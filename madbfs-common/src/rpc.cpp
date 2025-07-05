#include "madbfs-common/rpc.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/log.hpp"
#include "madbfs-common/util/overload.hpp"

#define HANDLE_ERROR(Res, Want, Msg)                                                                         \
    if (not(Res)) {                                                                                          \
        madbfs::log_e("{}: " Msg ": {}", __func__, Res.error().message());                                   \
        co_return madbfs::Unexpect{ madbfs::async::to_generic_err(Res.error(), Errc::not_connected) };       \
    } else if (Res.value() != Want) {                                                                        \
        madbfs::log_e("{}: " Msg ": message length mismatch [{} vs {}]", __func__, Res.value(), Want);       \
        co_return madbfs::Unexpect{ madbfs::Errc::broken_pipe };                                             \
    }

#define HANDLE_ERROR_ELSE(Res, Want, Msg, Else)                                                              \
    if (not(Res)) {                                                                                          \
        madbfs::log_e("{}: " Msg ": {}", __func__, Res.error().message());                                   \
        Else;                                                                                                \
    } else if (Res.value() != Want) {                                                                        \
        madbfs::log_e("{}: " Msg ": message length mismatch [{} vs {}]", __func__, Res.value(), Want);       \
        Else;                                                                                                \
    }

// NOTE: if only C++ has '?' a la rust... this is the most I can think of for C++
#define TRY(Name, Stmt)                                                                                      \
    auto Name = Stmt;                                                                                        \
    if (not Name) {                                                                                          \
        return std::nullopt;                                                                                 \
    }

namespace madbfs::rpc
{
    template <std::signed_integral I>
    timespec to_timespec(time_t sec, I nsec)
    {
        using slong = decltype(timespec::tv_nsec);
        return { .tv_sec = static_cast<time_t>(sec), .tv_nsec = static_cast<slong>(nsec) };
    }

    template <std::integral I>
    std::array<u8, sizeof(I)> to_net_bytes(I value)
    {
        if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {    // no std::endian on Android NDK
            return std::bit_cast<std::array<u8, sizeof(I)>>(value);
        } else {
            return std::bit_cast<std::array<u8, sizeof(I)>>(std::byteswap(value));
        }
    }

    template <std::integral I>
    I from_net_bytes(std::array<u8, sizeof(I)> bytes)
    {
        if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {    // no std::endian on Android NDK
            return std::bit_cast<I>(bytes);
        } else {
            return std::byteswap(std::bit_cast<I>(bytes));
        }
    }

    class PayloadBuilder
    {
    public:
        PayloadBuilder(Vec<u8>& buffer)
            : m_buffer{ buffer }
        {
        }

        template <std::integral I, typename Self>
        Self&& write_int(this Self&& self, I value)
        {
            auto array = to_net_bytes(value);
            self.m_buffer.insert(self.m_buffer.end(), array.begin(), array.end());
            return std::forward<Self>(self);
        }

        template <typename Self>
        Self&& write_procedure(this Self&& self, Procedure procedure)
        {
            return std::forward<Self>(self).write_int(static_cast<u8>(procedure));
        }

        template <typename Self>
        Self&& write_status(this Self&& self, Status status)
        {
            return std::forward<Self>(self).write_int(static_cast<u8>(status));
        }

        template <typename Self>
        Self&& write_id(this Self&& self, Id id)
        {
            return std::forward<Self>(self).write_int(id.inner());
        }

        template <typename Self>
        Self&& write_bytes(this Self&& self, Span<const u8> bytes)
        {
            self.template write_int<u64>(bytes.size());
            self.m_buffer.insert(self.m_buffer.end(), bytes.begin(), bytes.end());
            return std::forward<Self>(self);
        }

        template <typename Self>
        Self&& write_path(this Self&& self, Str path)
        {
            self.template write_int<u64>(path.size() + 1);
            auto span = Span{ reinterpret_cast<const u8*>(path.data()), path.size() };
            self.m_buffer.insert(self.m_buffer.end(), span.begin(), span.end());
            self.m_buffer.push_back(0x00);    // null terminator
            return std::forward<Self>(self);
        }

    protected:
        Vec<u8>& m_buffer;
    };

    class PayloadReader
    {
    public:
        PayloadReader(Span<const u8> buffer)
            : m_buffer{ buffer }
        {
        }

        template <std::integral I>
        Opt<I> read_int()
        {
            if (m_index + sizeof(I) - 1 >= m_buffer.size()) {
                return std::nullopt;
            }

            auto array = Array<u8, sizeof(I)>{};
            sr::copy_n(m_buffer.begin() + static_cast<isize>(m_index), array.size(), array.begin());
            auto value  = from_net_bytes<I>(array);
            m_index    += sizeof(I);
            return value;
        }

        Opt<Procedure> read_procedure()
        {
            return read_int<u8>().and_then([](u8 v) -> Opt<Procedure> {
                auto proc = Procedure{ v };
                switch (proc) {
                case Procedure::Listdir:
                case Procedure::Stat:
                case Procedure::Readlink:
                case Procedure::Mknod:
                case Procedure::Mkdir:
                case Procedure::Unlink:
                case Procedure::Rmdir:
                case Procedure::Rename:
                case Procedure::Truncate:
                case Procedure::Read:
                case Procedure::Write:
                case Procedure::Utimens:
                case Procedure::CopyFileRange: return proc;
                }
                return std::nullopt;
            });
        }

        Opt<Status> read_status()
        {
            return read_int<u8>().transform([](u8 v) { return Status{ v }; });
        }

        Opt<Id> read_id()
        {
            return read_int<Id::Inner>().transform([](Id::Inner v) { return Id{ v }; });
        }

        Opt<Span<const u8>> read_bytes()
        {
            return read_int<u64>().and_then([&](u64 size) -> Opt<Span<const u8>> {
                if (m_index + size - 1 >= m_buffer.size()) {
                    return std::nullopt;
                }
                auto span  = m_buffer.subspan(m_index, size);
                m_index   += size;
                return span;
            });
        }

        Opt<Str> read_path()
        {
            return read_int<u64>().and_then([&](u64 size) -> Opt<Str> {
                if (m_index + size - 1 >= m_buffer.size()) {
                    return std::nullopt;
                }
                auto span  = m_buffer.subspan(m_index, size - 1);    // without null terminator
                m_index   += size;
                return Str{ reinterpret_cast<const char*>(span.data()), span.size() };
            });
        }

    private:
        usize          m_index = 0;
        Span<const u8> m_buffer;
    };

    class RequestBuilder : public PayloadBuilder
    {
    public:
        using PayloadBuilder::write_bytes;
        using PayloadBuilder::write_int;
        using PayloadBuilder::write_path;

        RequestBuilder(Vec<u8>& buffer, Id id, Procedure proc)
            : PayloadBuilder{ buffer }
        {
            write_id(id).write_procedure(proc);
            write_int<u64>(0);    // will be filled later on build()
        }

        Span<const u8> build()
        {
            constexpr auto header_len = sizeof(Id) + sizeof(Procedure) + sizeof(u64);

            auto& buf  = m_buffer;
            auto  size = buf.size() - header_len;
            auto  arr  = to_net_bytes(static_cast<u64>(size));
            sr::copy_n(arr.begin(), arr.size(), buf.begin() + header_len - sizeof(u64));

            // auto str = Str{ reinterpret_cast<const char*>(buf.data()), buf.size() };
            // log_d("{}: request built: {:?}", __func__, str);

            return buf;
        }
    };

    class ResponseBuilder : public PayloadBuilder
    {
    public:
        using PayloadBuilder::write_bytes;
        using PayloadBuilder::write_int;
        using PayloadBuilder::write_path;

        ResponseBuilder(Vec<u8>& buffer, Id id, Procedure proc, Status status)
            : PayloadBuilder{ buffer }
        {
            write_id(id).write_procedure(proc).write_status(status);
            write_int<u64>(0);    // will be filled later on build()
        }

        Span<const u8> build()
        {
            constexpr auto header_len = sizeof(Id) + sizeof(Procedure) + sizeof(Status) + sizeof(u64);

            auto& buf  = m_buffer;
            auto  size = buf.size() - header_len;
            auto  arr  = to_net_bytes(static_cast<u64>(size));
            sr::copy_n(arr.begin(), arr.size(), buf.begin() + header_len - sizeof(u64));

            // auto str = Str{ reinterpret_cast<const char*>(buf.data()), buf.size() };
            // log_d("{}: response built: {:?}", __func__, str);

            return buf;
        }
    };
}

namespace madbfs::rpc
{
    Opt<Response> parse_response(Span<const u8> buffer, Procedure proc)
    {
        auto reader = PayloadReader{ buffer };

        switch (proc) {
        case Procedure::Listdir: {
            TRY(size, reader.read_int<u64>());

            auto entries = Vec<Pair<Str, resp::Stat>>{};
            entries.reserve(*size);

            for (auto _ : sv::iota(0uz, *size)) {
                TRY(path, reader.read_path());
                TRY(size, reader.read_int<i64>());
                TRY(links, reader.read_int<u64>());
                TRY(mtime_sec, reader.read_int<i64>());
                TRY(mtime_nsec, reader.read_int<i64>());
                TRY(atime_sec, reader.read_int<i64>());
                TRY(atime_nsec, reader.read_int<i64>());
                TRY(ctime_sec, reader.read_int<i64>());
                TRY(ctime_nsec, reader.read_int<i64>());
                TRY(mode, reader.read_int<u32>());
                TRY(uid, reader.read_int<u32>());
                TRY(gid, reader.read_int<u32>());

                entries.emplace_back(
                    *path,
                    resp::Stat{
                        .size  = static_cast<off_t>(*size),
                        .links = static_cast<nlink_t>(*links),
                        .mtime = to_timespec(*mtime_sec, *mtime_nsec),
                        .atime = to_timespec(*atime_sec, *atime_nsec),
                        .ctime = to_timespec(*ctime_sec, *ctime_nsec),
                        .mode  = static_cast<mode_t>(*mode),
                        .uid   = static_cast<uid_t>(*uid),
                        .gid   = static_cast<uid_t>(*gid),
                    }
                );
            }

            return resp::Listdir{ .entries = std::move(entries) };
        } break;

        case Procedure::Stat: {
            TRY(size, reader.read_int<i64>());
            TRY(links, reader.read_int<u64>());
            TRY(mtime_sec, reader.read_int<i64>());
            TRY(mtime_nsec, reader.read_int<i64>());
            TRY(atime_sec, reader.read_int<i64>());
            TRY(atime_nsec, reader.read_int<i64>());
            TRY(ctime_sec, reader.read_int<i64>());
            TRY(ctime_nsec, reader.read_int<i64>());
            TRY(mode, reader.read_int<u32>());
            TRY(uid, reader.read_int<u32>());
            TRY(gid, reader.read_int<u32>());

            return resp::Stat{
                .size  = static_cast<off_t>(*size),
                .links = static_cast<nlink_t>(*links),
                .mtime = to_timespec(*mtime_sec, *mtime_nsec),
                .atime = to_timespec(*atime_sec, *atime_nsec),
                .ctime = to_timespec(*ctime_sec, *ctime_nsec),
                .mode  = static_cast<mode_t>(*mode),
                .uid   = static_cast<uid_t>(*uid),
                .gid   = static_cast<uid_t>(*gid),
            };
        } break;

        case Procedure::Readlink: {
            TRY(path, reader.read_path());
            return resp::Readlink{ .target = *path };
        } break;

        case Procedure::Mknod: return resp::Mknod{};
        case Procedure::Mkdir: return resp::Mkdir{};
        case Procedure::Unlink: return resp::Unlink{};
        case Procedure::Rmdir: return resp::Rmdir{};
        case Procedure::Rename: return resp::Rename{};
        case Procedure::Truncate: return resp::Truncate{};

        case Procedure::Read: {
            TRY(bytes, reader.read_bytes());
            return resp::Read{ .read = *bytes };
        } break;

        case Procedure::Write: {
            TRY(size, reader.read_int<u64>());
            return resp::Write{ .size = static_cast<usize>(*size) };
        } break;

        case Procedure::Utimens: return resp::Utimens{};

        case Procedure::CopyFileRange: {
            TRY(size, reader.read_int<u64>());
            return resp::CopyFileRange{ .size = static_cast<usize>(*size) };
        } break;
        }

        return std::nullopt;
    }

    Await<void> Client::start()
    {
        m_running = true;

        auto exec = co_await async::current_executor();
        async::spawn(exec, receive(), [&](std::exception_ptr e, Expect<void> res) {
            m_running = false;

            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (std::exception& e) {
                    log_c("receive: exception occurred: {}", e.what());
                }
            } else if (not res) {
                auto msg = std::make_error_code(res.error()).message();
                log_e("receive: finished with error: {}", msg);
            }

            log_e("receive: there are {} promises unhandled", m_requests.size());
            for (auto& [id, promise] : m_requests) {
                promise.promise.set_value(Unexpect{ e ? Errc::state_not_recoverable : Errc::not_connected });
            }

            m_requests.clear();
        });

        async::spawn(exec, send(), [&](std::exception_ptr e, Expect<void> res) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (std::exception& e) {
                    log_c("send: exception occurred: {}", e.what());
                }
            } else if (not res) {
                auto msg = std::make_error_code(res.error()).message();
                log_e("send: finished with error: {}", msg);
            }

            m_channel.reset();
        });
    }

    AExpect<void> Client::receive()
    {
        while (m_running) {
            constexpr auto header_len = sizeof(Id) + sizeof(Procedure) + sizeof(Status) + sizeof(u64);

            auto header = Array<u8, header_len>{};
            auto n      = co_await async::read_exact<u8>(m_socket, header);
            HANDLE_ERROR(n, header_len, "failed to read response header");

            auto reader = PayloadReader{ header };
            auto id     = reader.read_id().value();
            auto proc   = reader.read_procedure();    // can fail, invalid procedure
            auto status = reader.read_status().value();
            auto size   = reader.read_int<u64>().value();

            if (not proc) {
                log_d("{}: RESP RECV {} [invalid procedure]", __func__, id.inner());
                auto buffer = Vec<u8>(size);
                std::ignore = co_await async::read_exact<u8>(m_socket, buffer);
                continue;
            }

            log_d("{}: RESP RECV {} [{}]", __func__, id.inner(), to_string(*proc));

            auto req = m_requests.extract(id);
            if (req.empty()) {
                log_e("{}: response incoming for id {} but no promise registered", __func__, id.inner());
                auto buffer = Vec<u8>(size);
                std::ignore = co_await async::read_exact<u8>(m_socket, buffer);
                continue;
            }

            auto& [buffer, promise] = req.mapped();
            if (status != Status::Success) {
                promise.set_value(Unexpect{ static_cast<Errc>(status) });
                continue;
            }

            buffer.resize(size);
            auto n1 = co_await async::read_exact<u8>(m_socket, buffer);
            HANDLE_ERROR_ELSE(n1, buffer.size(), "failed to read response payload", continue);

            auto response = parse_response(buffer, *proc);
            if (not response) {
                log_e("{}: [{}] failed to parse response", __func__, id.inner());
                promise.set_value(Unexpect{ Errc::bad_message });
                continue;
            }

            promise.set_value(std::move(response).value());
        }
        co_return Expect<void>{};
    }

    AExpect<void> Client::send()
    {
        while (m_running and m_channel.is_open()) {
            auto payload = co_await m_channel.async_receive(async::as_expected(async::use_awaitable));
            if (not payload) {
                log_e("{}: failed to recv payload from channel: {}", __func__, payload.error().message());
                co_return Unexpect{ async::to_generic_err(payload.error(), Errc::broken_pipe) };
            }

            auto n = co_await async::write_exact(m_socket, payload.value());
            HANDLE_ERROR_ELSE(n, payload->size(), "failed to send request payload", continue);
        }

        co_return Expect<void>{};
    }

    void Client::stop()
    {
        m_running = false;
        m_socket.cancel();
        m_socket.close();
    }

    AExpect<Client::Future> Client::send_req(Vec<u8>& buffer, Request req)
    {
        if (not m_running) {
            co_return Unexpect{ Errc::not_connected };
        }

        buffer.clear();

        auto id      = Id{ ++m_counter };
        auto proc    = static_cast<Procedure>(req.index());
        auto builder = RequestBuilder{ buffer, id, proc };

        log_d("{}: REQ  SENT {} [{}]", __func__, id.inner(), to_string(proc));

        auto overload = util::Overload{
            [&](req::Mknod&& req) {
                auto [path, mode, dev] = req;
                builder    //
                    .write_path(path)
                    .write_int<u32>(mode)
                    .write_int<u64>(dev);
            },
            [&](req::Mkdir&& req) {
                auto [path, mode] = req;
                builder    //
                    .write_path(path)
                    .write_int<u32>(mode);
            },
            [&](req::Rename&& req) {
                auto [from, to, flags] = req;
                builder    //
                    .write_path(from)
                    .write_path(to)
                    .write_int<u32>(flags);
            },
            [&](req::Truncate&& req) {
                auto [path, size] = req;
                builder    //
                    .write_path(path)
                    .write_int<i64>(size);
            },
            [&](req::Read&& req) {
                auto [path, offset, size] = req;
                builder    //
                    .write_path(path)
                    .write_int<i64>(offset)
                    .write_int<u64>(size);
            },
            [&](req::Write&& req) {
                auto [path, offset, in] = req;
                builder    //
                    .write_path(path)
                    .write_int<i64>(offset)
                    .write_bytes(in);
            },
            [&](req::Utimens&& req) {
                auto [path, atime, mtime] = req;
                builder    //
                    .write_path(path)
                    .write_int<i64>(atime.tv_sec)
                    .write_int<i64>(atime.tv_nsec)
                    .write_int<i64>(mtime.tv_sec)
                    .write_int<i64>(mtime.tv_nsec);
            },
            [&](req::CopyFileRange&& req) {
                auto [in_path, in_off, out_path, out_off, size] = req;
                builder    //
                    .write_path(in_path)
                    .write_int<i64>(in_off)
                    .write_path(out_path)
                    .write_int<i64>(out_off)
                    .write_int<u64>(size);
            },
            [&](auto&& req) {
                auto [path] = req;
                builder.write_path(path);
            },
        };

        auto promise = saf::promise<Expect<Response>>{ co_await async::current_executor() };
        auto future  = promise.get_future();

        m_requests.emplace(id, Promise{ buffer, std::move(promise) });

        std::visit(std::move(overload), std::move(req));
        auto payload = builder.build();

        auto res = co_await m_channel.async_send({}, payload, async::as_expected(async::use_awaitable));
        if (not res) {
            log_e("{}: failed to send payload to channel: {}", __func__, res.error().message());
            co_return Unexpect{ async::to_generic_err(res.error(), Errc::broken_pipe) };
        }

        co_return future;
    }
}

namespace madbfs::rpc
{
    Opt<Request> parse_request(Span<const u8> buffer, Procedure proc)
    {
        auto reader = PayloadReader{ buffer };

        switch (proc) {
        case Procedure::Listdir: {
            TRY(path, reader.read_path());
            return req::Listdir{ .path = *path };
        }

        case Procedure::Stat: {
            TRY(path, reader.read_path());
            return req::Stat{ .path = *path };
        }

        case Procedure::Readlink: {
            TRY(path, reader.read_path());
            return req::Readlink{ .path = *path };
        }

        case Procedure::Mknod: {
            TRY(path, reader.read_path());
            TRY(mode, reader.read_int<u32>());
            TRY(dev, reader.read_int<u64>());
            return req::Mknod{
                .path = *path,
                .mode = static_cast<mode_t>(*mode),
                .dev  = static_cast<dev_t>(*dev),
            };
        }

        case Procedure::Mkdir: {
            TRY(path, reader.read_path());
            TRY(mode, reader.read_int<u32>());
            return req::Mkdir{ .path = *path, .mode = static_cast<mode_t>(*mode) };
        }

        case Procedure::Unlink: {
            TRY(path, reader.read_path());
            return req::Unlink{ .path = *path };
        }

        case Procedure::Rmdir: {
            TRY(path, reader.read_path());
            return req::Rmdir{ .path = *path };
        }

        case Procedure::Rename: {
            TRY(from, reader.read_path());
            TRY(to, reader.read_path());
            TRY(flags, reader.read_int<u32>());
            return req::Rename{ .from = *from, .to = *to, .flags = *flags };
        }

        case Procedure::Truncate: {
            TRY(path, reader.read_path());
            TRY(size, reader.read_int<i64>());
            return req::Truncate{ .path = *path, .size = static_cast<off_t>(*size) };
        }

        case Procedure::Read: {
            TRY(path, reader.read_path());
            TRY(offset, reader.read_int<i64>());
            TRY(size, reader.read_int<u64>());
            return req::Read{
                .path   = *path,
                .offset = static_cast<off_t>(*offset),
                .size   = static_cast<usize>(*size),
            };
        }

        case Procedure::Write: {
            TRY(path, reader.read_path());
            TRY(offset, reader.read_int<i64>());
            TRY(bytes, reader.read_bytes());
            return req::Write{ .path = *path, .offset = static_cast<off_t>(*offset), .in = *bytes };
        }

        case Procedure::Utimens: {
            TRY(path, reader.read_path());
            TRY(atime_sec, reader.read_int<i64>());
            TRY(atime_nsec, reader.read_int<i64>());
            TRY(mtime_sec, reader.read_int<i64>());
            TRY(mtime_nsec, reader.read_int<i64>());
            return req::Utimens{
                .path  = *path,
                .atime = to_timespec(*atime_sec, *atime_nsec),
                .mtime = to_timespec(*mtime_sec, *mtime_nsec),
            };
        }

        case Procedure::CopyFileRange: {
            TRY(in_path, reader.read_path());
            TRY(in_offset, reader.read_int<i64>());
            TRY(out_path, reader.read_path());
            TRY(out_offset, reader.read_int<i64>());
            TRY(size, reader.read_int<u64>());
            return req::CopyFileRange{
                .in_path    = *in_path,
                .in_offset  = static_cast<off_t>(*in_offset),
                .out_path   = *out_path,
                .out_offset = static_cast<off_t>(*out_offset),
                .size       = static_cast<usize>(*size),
            };
        } break;
        }

        return std::nullopt;
    }

    AExpect<void> Server::listen(Handler handler)
    {
        m_running = true;

        while (m_running) {
            constexpr auto header_len = sizeof(Id) + sizeof(Procedure) + sizeof(u64);

            auto header = Array<u8, header_len>{};
            auto n      = co_await async::read_exact<u8>(m_socket, header);
            HANDLE_ERROR(n, header_len, "failed to read request header");

            // auto str = Str{ reinterpret_cast<const char*>(header.data()), header.size() };
            // log_d("{}: header: {:?}", __func__, str);

            auto reader = PayloadReader{ header };
            auto id     = reader.read_id().value();
            auto proc   = reader.read_procedure();    // can fail, invalid procedure
            auto size   = reader.read_int<u64>().value();

            if (not proc) {
                log_d("{}: recv req: id={} | proc=[invalid] | size={}", __func__, id.inner(), size);
                auto buffer = Vec<u8>(size);
                std::ignore = co_await async::read_exact<u8>(m_socket, buffer);
                continue;
            }

            log_d("{}: recv req id={} | proc={} | size={}", __func__, id.inner(), to_string(*proc), size);

            auto buffer = Vec<u8>(size);
            auto n1     = co_await async::read_exact<u8>(m_socket, buffer);
            HANDLE_ERROR(n1, buffer.size(), "failed to read request payload");

            // str = Str{ reinterpret_cast<const char*>(buffer.data()), buffer.size() };
            // log_d("{}: [{}] payload: {:?}", __func__, id.inner(), str);

            auto request = parse_request(buffer, *proc);
            if (not request) {
                log_e("{}: [{}] failed to parse request", __func__, id.inner());
                continue;
            }

            auto exec = co_await async::current_executor();
            auto coro = [&, id, proc, r = std::move(request), b = std::move(buffer)] mutable -> Await<void> {
                auto response = co_await handler(b, std::move(r).value());
                std::ignore   = co_await send_resp(id, *proc, std::move(response));
            };

            async::spawn(exec, coro(), async::detached);
        }

        co_return Expect<void>{};
    }

    AExpect<void> Server::send_resp(Id id, Procedure proc, Var<Status, Response> response)
    {
        auto status = Status::Success;
        if (auto err = std::get_if<Status>(&response); err) {
            status = *err;
        }

        auto buffer  = Vec<u8>{};
        auto builder = ResponseBuilder{ buffer, id, proc, status };

        if (status != Status::Success) {
            auto payload = builder.build();
            auto n       = co_await async::write_exact(m_socket, payload);
            HANDLE_ERROR(n, payload.size(), "failed to send response payload");
            co_return Expect<void>{};
        }

        auto overload = util::Overload{
            [&](resp::Listdir&& resp) {
                builder.write_int<u64>(resp.entries.size());
                for (const auto& [name, stat] : resp.entries) {
                    builder    //
                        .write_path(name)
                        .write_int<i64>(stat.size)
                        .write_int<u64>(stat.links)
                        .write_int<i64>(stat.mtime.tv_sec)
                        .write_int<i64>(stat.mtime.tv_nsec)
                        .write_int<i64>(stat.atime.tv_sec)
                        .write_int<i64>(stat.atime.tv_nsec)
                        .write_int<i64>(stat.ctime.tv_sec)
                        .write_int<i64>(stat.ctime.tv_nsec)
                        .write_int<u32>(stat.mode)
                        .write_int<u32>(stat.uid)
                        .write_int<u32>(stat.gid);
                }
            },
            [&](resp::Stat&& resp) {
                builder    //
                    .write_int<i64>(resp.size)
                    .write_int<u64>(resp.links)
                    .write_int<i64>(resp.mtime.tv_sec)
                    .write_int<i64>(resp.mtime.tv_nsec)
                    .write_int<i64>(resp.atime.tv_sec)
                    .write_int<i64>(resp.atime.tv_nsec)
                    .write_int<i64>(resp.ctime.tv_sec)
                    .write_int<i64>(resp.ctime.tv_nsec)
                    .write_int<u32>(resp.mode)
                    .write_int<u32>(resp.uid)
                    .write_int<u32>(resp.gid);
            },
            [&](resp::Readlink&& resp) { builder.write_path(resp.target); },
            [&](resp::Mknod&&) { /* empty */ },
            [&](resp::Mkdir&&) { /* empty */ },
            [&](resp::Unlink&&) { /* empty */ },
            [&](resp::Rmdir&&) { /* empty */ },
            [&](resp::Rename&&) { /* empty */ },
            [&](resp::Truncate&&) { /* empty */ },
            [&](resp::Read&& resp) { builder.write_bytes(resp.read); },
            [&](resp::Write&& resp) { builder.write_int<u64>(resp.size); },
            [&](resp::Utimens&&) { /* empty */ },
            [&](resp::CopyFileRange&& resp) { builder.write_int<u64>(resp.size); },
        };

        auto resp = std::get<Response>(std::move(response));
        if (auto actual = static_cast<Procedure>(resp.index()); actual != proc) {
            log_e("{}: mismatched procedure: [{} vs {}]", __func__, to_string(actual), to_string(proc));
            co_return Unexpect{ Errc::bad_message };
        }

        std::visit(std::move(overload), std::move(resp));

        auto payload = builder.build();
        auto n       = co_await async::write_exact(m_socket, payload);
        HANDLE_ERROR(n, payload.size(), "failed to send response payload");
        co_return Expect<void>{};
    }
}

namespace madbfs::rpc
{
    Str to_string(Procedure procedure)
    {
        switch (procedure) {
        case Procedure::Listdir: return "Listdir";
        case Procedure::Stat: return "Stat";
        case Procedure::Readlink: return "Readlink";
        case Procedure::Mknod: return "Mknod";
        case Procedure::Mkdir: return "Mkdir";
        case Procedure::Unlink: return "Unlink";
        case Procedure::Rmdir: return "Rmdir";
        case Procedure::Rename: return "Rename";
        case Procedure::Truncate: return "Truncate";
        case Procedure::Read: return "Read";
        case Procedure::Write: return "Write";
        case Procedure::Utimens: return "Utimens";
        case Procedure::CopyFileRange: return "CopyFileRange";
        }

        return "Unknown";
    }

    Str to_string(Request request)
    {
        auto index = request.index();
        return to_string(static_cast<Procedure>(index));
    }

    Str to_string(Response response)
    {
        auto index = response.index();
        return to_string(static_cast<Procedure>(index));
    }

    AExpect<void> handshake(Socket& sock, bool client)
    {
        if (client) {
            auto n = co_await async::write_exact<char>(sock, server_ready_string);
            HANDLE_ERROR(n, server_ready_string.size(), "failed to send handshake to server");

            auto buffer = String(server_ready_string.size(), '\0');
            auto n1     = co_await async::read_exact<char>(sock, buffer);
            HANDLE_ERROR(n1, buffer.size(), "failed to read handshake from server");

            if (buffer != server_ready_string) {
                co_return Unexpect{ Errc::bad_message };
            }
        } else {
            auto buffer = String(server_ready_string.size(), '\0');
            auto n1     = co_await async::read_exact<char>(sock, buffer);
            HANDLE_ERROR(n1, buffer.size(), "failed to read handshake from client");

            if (buffer != server_ready_string) {
                co_return Unexpect{ Errc::bad_message };
            }

            auto n2 = co_await async::write_exact<char>(sock, server_ready_string);
            HANDLE_ERROR(n2, server_ready_string.size(), "failed to send handshake to client");
        }

        co_return Expect<void>{};
    }
}
