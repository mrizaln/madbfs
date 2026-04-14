#include "madbfs-common/rpc.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/log.hpp"

#include <bit>

// error handling that adapts error_code into errc
#define HANDLE_ERROR(Res, Want, Msg)                                                                         \
    if (not(Res)) {                                                                                          \
        madbfs::log_e("{}: " Msg ": {}", __func__, Res.error().message());                                   \
        co_return madbfs::Unexpect{ madbfs::async::to_generic_err(Res.error(), Errc::not_connected) };       \
    } else if (Res.value() != Want) {                                                                        \
        madbfs::log_e("{}: " Msg ": message length mismatch [{} vs {}]", __func__, Res.value(), Want);       \
        co_return madbfs::Unexpect{ madbfs::Errc::bad_message };                                             \
    }

// error handling that adapts error_code into errc with custom handling on failure
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

namespace
{
    using namespace madbfs::aliases;
    using namespace madbfs::rpc;

    /**
     * @brief Simple wrapper to convert `time_t` + nsec into `timespec`.
     *
     * @param sec Seconds in `time_t.
     * @param nsec Nanoseconds.
     *
     * @return Resulting `timespec`.
     */
    template <std::signed_integral I>
    timespec to_timespec(time_t sec, I nsec)
    {
        using slong = decltype(timespec::tv_nsec);
        return { .tv_sec = static_cast<time_t>(sec), .tv_nsec = static_cast<slong>(nsec) };
    }

    /**
     * @brief Convert an integral into bytes in network order.
     *
     * @param value Integer.
     *
     * @return Resulting bytes in network order.
     */
    template <std::integral I>
    Array<u8, sizeof(I)> to_net_bytes(I value)
    {
        if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {    // no std::endian on Android NDK
            return std::bit_cast<Array<u8, sizeof(I)>>(value);
        } else {
            return std::bit_cast<Array<u8, sizeof(I)>>(std::byteswap(value));
        }
    }

    /**
     * @brief Convert a span of bytes into an integral using betwork order.
     *
     * @tparam I The desired integral type.
     *
     * @param bytes Input bytes with the size of `I` in bytes.
     *
     * @return Resulting integral.
     */
    template <std::integral I>
    I from_net_bytes(Array<u8, sizeof(I)> bytes)
    {
        if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {    // no std::endian on Android NDK
            return std::bit_cast<I>(bytes);
        } else {
            return std::byteswap(std::bit_cast<I>(bytes));
        }
    }

    /**
     * @class PayloadBuilder
     * @brief Simple payload builder.
     */
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
            return std::forward<Self>(self).write_int(static_cast<i32>(status));
        }

        template <typename Self>
        Self&& write_id(this Self&& self, Id id)
        {
            return std::forward<Self>(self).write_int(id.inner());
        }

        template <typename Self>
        Self&& write_open_mode(this Self&& self, OpenMode mode)
        {
            return std::forward<Self>(self).write_int(static_cast<u8>(mode));
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
            self.template write_int<u64>(path.size() + 1);    // with null terminator
            auto span = Span{ reinterpret_cast<const u8*>(path.data()), path.size() };
            self.m_buffer.insert(self.m_buffer.end(), span.begin(), span.end());
            self.m_buffer.push_back(0x00);    // null terminator
            return std::forward<Self>(self);
        }

    protected:
        Vec<u8>& m_buffer;
    };

    /**
     * @class PayloadReader
     * @brief Simple payload reader.
     */
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
                case Procedure::Stat:
                case Procedure::Listdir:
                case Procedure::Readlink:
                case Procedure::Mknod:
                case Procedure::Mkdir:
                case Procedure::Unlink:
                case Procedure::Rmdir:
                case Procedure::Rename:
                case Procedure::Truncate:
                case Procedure::Utimens:
                case Procedure::CopyFileRange:
                case Procedure::Open:
                case Procedure::Close:
                case Procedure::Read:
                case Procedure::Write:
                case Procedure::Ping: return proc;
                }
                return std::nullopt;
            });
        }

        Opt<Status> read_status()
        {
            return read_int<i32>().transform([](u8 v) { return Status{ v }; });
        }

        Opt<Id> read_id()
        {
            return read_int<Id::Inner>().transform([](Id::Inner v) { return Id{ v }; });
        }

        Opt<OpenMode> read_open_mode()
        {
            return read_int<u8>().transform([](u8 v) { return static_cast<OpenMode>(v); });
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

    /**
     * @class RequestBuilder
     * @brief Simple payload builder for request.
     */
    class RequestBuilder : public PayloadBuilder
    {
    public:
        using PayloadBuilder::write_bytes;
        using PayloadBuilder::write_int;
        using PayloadBuilder::write_path;

        static constexpr auto header_size = sizeof(Id) + sizeof(Procedure) + sizeof(u64);

        RequestBuilder(Vec<u8>& buffer, Id id, Procedure proc)
            : PayloadBuilder{ buffer }
        {
            write_id(id).write_procedure(proc);
            write_int<u64>(0);    // will be filled later on build()
        }

        Span<const u8> build()
        {
            auto& buf  = m_buffer;
            auto  size = buf.size() - header_size;
            auto  arr  = to_net_bytes(static_cast<u64>(size));
            sr::copy_n(arr.begin(), arr.size(), buf.begin() + header_size - sizeof(u64));

            // auto str = Str{ reinterpret_cast<const char*>(buf.data()), buf.size() };
            // log_d("{}: request built: {:?}", __func__, str);

            return buf;
        }
    };

    /**
     * @class ResponseBuilder
     * @brief Simple payload builder for response.
     */
    class ResponseBuilder : public PayloadBuilder
    {
    public:
        using PayloadBuilder::write_bytes;
        using PayloadBuilder::write_int;
        using PayloadBuilder::write_path;

        static constexpr auto header_size = sizeof(Id) + sizeof(Procedure) + sizeof(Status) + sizeof(u64);

        ResponseBuilder(Vec<u8>& buffer, Id id, Procedure proc, Status status)
            : PayloadBuilder{ buffer }
        {
            write_id(id).write_procedure(proc).write_status(status);
            write_int<u64>(0);    // will be filled later on build()
        }

        Span<const u8> build()
        {

            auto& buf  = m_buffer;
            auto  size = buf.size() - header_size;
            auto  arr  = to_net_bytes(static_cast<u64>(size));
            sr::copy_n(arr.begin(), arr.size(), buf.begin() + header_size - sizeof(u64));

            // auto str = Str{ reinterpret_cast<const char*>(buf.data()), buf.size() };
            // log_d("{}: response built: {:?}", __func__, str);

            return buf;
        }
    };
}

namespace madbfs::rpc
{
    Str to_string(Procedure procedure)
    {
        switch (procedure) {
        case Procedure::Stat: return "Stat";
        case Procedure::Listdir: return "Listdir";
        case Procedure::Readlink: return "Readlink";
        case Procedure::Mknod: return "Mknod";
        case Procedure::Mkdir: return "Mkdir";
        case Procedure::Unlink: return "Unlink";
        case Procedure::Rmdir: return "Rmdir";
        case Procedure::Rename: return "Rename";
        case Procedure::Truncate: return "Truncate";
        case Procedure::Utimens: return "Utimens";
        case Procedure::CopyFileRange: return "CopyFileRange";
        case Procedure::Open: return "Open";
        case Procedure::Close: return "Close";
        case Procedure::Read: return "Read";
        case Procedure::Write: return "Write";
        case Procedure::Ping: return "Ping";
        }

        return "Unknown";
    }

    Str to_string(Request request)
    {
        return to_string(request.proc());
    }

    Str to_string(Response response)
    {
        return to_string(response.proc());
    }

    AExpect<void> handshake(Socket& sock)
    {
        const auto message = std::format("{}:{}\n", server_ready_string, MADBFS_VERSION_STRING);

        auto n = co_await async::write_lv<char>(sock, message);
        HANDLE_ERROR(n, message.size(), "failed to send handshake to server");

        auto buffer = String(message.size(), '\0');
        auto n1     = co_await async::read_lv<char>(sock, buffer);
        HANDLE_ERROR(n1, buffer.size(), "failed to read handshake from server");

        if (not sr::equal(buffer, message)) {
            log_e("mismatched message: [{:?} vs {:?}]", buffer, message);
            co_return Unexpect{ Errc::bad_message };
        }

        co_return Expect<void>{};
    }

    Span<const u8> build_request(Vec<u8>& buffer, const Request& req, Id id)
    {
        buffer.clear();

        auto builder = RequestBuilder{ buffer, id, req.proc() };

        return req.visit(Overload{
            [&](req::Stat req) {
                return builder    //
                    .write_path(req.path)
                    .build();
            },
            [&](req::Listdir req) {
                return builder    //
                    .write_path(req.path)
                    .build();
            },
            [&](req::Readlink req) {
                return builder    //
                    .write_path(req.path)
                    .build();
            },
            [&](req::Mknod req) {
                return builder    //
                    .write_path(req.path)
                    .write_int<u32>(req.mode)
                    .write_int<u64>(req.dev)
                    .build();
            },
            [&](req::Mkdir req) {
                return builder    //
                    .write_path(req.path)
                    .write_int<u32>(req.mode)
                    .build();
            },
            [&](req::Unlink req) {
                return builder    //
                    .write_path(req.path)
                    .build();
            },
            [&](req::Rmdir req) {
                return builder    //
                    .write_path(req.path)
                    .build();
            },
            [&](req::Rename req) {
                return builder    //
                    .write_path(req.from)
                    .write_path(req.to)
                    .write_int<u32>(req.flags)
                    .build();
            },
            [&](req::Truncate req) {
                return builder    //
                    .write_path(req.path)
                    .write_int<i64>(req.size)
                    .build();
            },
            [&](req::Utimens req) {
                return builder    //
                    .write_path(req.path)
                    .write_int<i64>(req.atime.tv_sec)
                    .write_int<i64>(req.atime.tv_nsec)
                    .write_int<i64>(req.mtime.tv_sec)
                    .write_int<i64>(req.mtime.tv_nsec)
                    .build();
            },
            [&](req::CopyFileRange req) {
                return builder    //
                    .write_path(req.in_path)
                    .write_int<i64>(req.in_offset)
                    .write_path(req.out_path)
                    .write_int<i64>(req.out_offset)
                    .write_int<u64>(req.size)
                    .build();
            },
            [&](req::Open req) {
                return builder    //
                    .write_path(req.path)
                    .write_open_mode(req.mode)
                    .build();
            },
            [&](req::Close req) {
                return builder    //
                    .write_int<u64>(req.fd)
                    .build();
            },
            [&](req::Read req) {
                return builder    //
                    .write_int<u64>(req.fd)
                    .write_int<i64>(req.offset)
                    .write_int<u64>(req.size)
                    .build();
            },
            [&](req::Write req) {
                return builder    //
                    .write_int<u64>(req.fd)
                    .write_int<i64>(req.offset)
                    .write_bytes(req.in)
                    .build();
            },
            [&](req::Ping req) {
                return builder    //
                    .write_int<u64>(req.num)
                    .build();
            },
        });
    }

    Span<const u8> build_response(Vec<u8>& buffer, Procedure proc, Var<Status, Response> response, Id id)
    {
        buffer.clear();

        if (auto err = std::get_if<0>(&response); err) {
            return ResponseBuilder{ buffer, id, proc, *err }.build();
        }

        auto builder = ResponseBuilder{ buffer, id, proc, Status{} };

        const auto& resp = *std::get_if<1>(&response);

        return resp.visit(Overload{
            [&](const resp::Stat& resp) {
                return builder    //
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
                    .write_int<u32>(resp.gid)
                    .build();
            },
            [&](const resp::Listdir& resp) {
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
                return builder.build();
            },
            // clang-format off
            [&](const resp::Readlink&      resp) { return builder.write_path(resp.target).build();   },
            [&](const resp::Mknod&             ) { return builder.build();                           },
            [&](const resp::Mkdir&             ) { return builder.build();                           },
            [&](const resp::Unlink&            ) { return builder.build();                           },
            [&](const resp::Rmdir&             ) { return builder.build();                           },
            [&](const resp::Rename&            ) { return builder.build();                           },
            [&](const resp::Truncate&          ) { return builder.build();                           },
            [&](const resp::Utimens&           ) { return builder.build();                           },
            [&](const resp::CopyFileRange& resp) { return builder.write_int<u64>(resp.size).build(); },
            [&](const resp::Open&          resp) { return builder.write_int<u64>(resp.fd  ).build(); },
            [&](const resp::Close&             ) { return builder.build();                           },
            [&](const resp::Read&          resp) { return builder.write_bytes   (resp.read).build(); },
            [&](const resp::Write&         resp) { return builder.write_int<u64>(resp.size).build(); },
            [&](const resp::Ping&          resp) { return builder.write_int<u64>(resp.num ).build(); },
            // clang-format on
        });
    }

    /**
     * @brief Parse raw buffer into request of desired procedure.
     *
     * @param buffer Input buffer.
     * @param proc Desired buffer.
     *
     * @return The request on success or `std::nullopt` if the buffer is not a payload for desired procedure,
     * the payload is incomplete, or the payload not containing the correct values for the procedure.
     *
     * This function must only be used for non-Ping procedures. If Ping is provided, the function will return
     * std::nullopt.
     */
    Opt<Request> parse_request(Span<const u8> buffer, Procedure proc)
    {
        auto reader = PayloadReader{ buffer };

        switch (proc) {
        case Procedure::Stat: {
            TRY(path, reader.read_path());
            return req::Stat{ .path = *path };
        }

        case Procedure::Listdir: {
            TRY(path, reader.read_path());
            return req::Listdir{ .path = *path };
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
        }

        case Procedure::Open: {
            TRY(path, reader.read_path());
            TRY(mode, reader.read_open_mode());
            return req::Open{ .path = *path, .mode = *mode };
        }

        case Procedure::Close: {
            TRY(fd, reader.read_int<u64>());
            return req::Close{ .fd = *fd };
        }

        case Procedure::Read: {
            TRY(fd, reader.read_int<u64>());
            TRY(offset, reader.read_int<i64>());
            TRY(size, reader.read_int<u64>());
            return req::Read{
                .fd     = *fd,
                .offset = static_cast<off_t>(*offset),
                .size   = static_cast<usize>(*size),
            };
        }

        case Procedure::Write: {
            TRY(fd, reader.read_int<u64>());
            TRY(offset, reader.read_int<i64>());
            TRY(bytes, reader.read_bytes());
            return req::Write{ .fd = *fd, .offset = static_cast<off_t>(*offset), .in = *bytes };
        }

        case Procedure::Ping: {
            TRY(num, reader.read_int<u64>())
            return req::Ping{ .num = *num };
        }
        }

        return std::nullopt;
    }

    /**
     * @brief Parse raw buffer info response of desired procedure.
     *
     * @param buffer Input buffer.
     * @param proc Desired procedure.
     *
     * @return The response on success or `std::nullopt` if the buffer is not a payload for desired procedure,
     * the payload is incomplete, or the payload not containing the correct values for the procedure.
     *
     * This function must only be used for non-Ping procedures. If Ping is provided, the function will return
     * std::nullopt.
     */
    Opt<Response> parse_response(Span<const u8> buffer, Procedure proc)
    {
        auto reader = PayloadReader{ buffer };

        switch (proc) {
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
        }

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
        }

        case Procedure::Readlink: {
            TRY(path, reader.read_path());
            return resp::Readlink{ .target = *path };
        }

        case Procedure::Mknod: return resp::Mknod{};
        case Procedure::Mkdir: return resp::Mkdir{};
        case Procedure::Unlink: return resp::Unlink{};
        case Procedure::Rmdir: return resp::Rmdir{};
        case Procedure::Rename: return resp::Rename{};
        case Procedure::Truncate: return resp::Truncate{};
        case Procedure::Utimens: return resp::Utimens{};

        case Procedure::CopyFileRange: {
            TRY(size, reader.read_int<u64>());
            return resp::CopyFileRange{ .size = static_cast<usize>(*size) };
        }

        case Procedure::Open: {
            TRY(fd, reader.read_int<u64>());
            return resp::Open{ .fd = *fd };
        }

        case Procedure::Close: return resp::Close{};

        case Procedure::Read: {
            TRY(bytes, reader.read_bytes());
            return resp::Read{ .read = *bytes };
        }

        case Procedure::Write: {
            TRY(size, reader.read_int<u64>());
            return resp::Write{ .size = static_cast<usize>(*size) };
        }

        case Procedure::Ping: {
            TRY(num, reader.read_int<u64>())
            return resp::Ping{ .num = *num };
        }
        }

        return std::nullopt;
    }

    AExpect<void> send_request(Socket& socket, Vec<u8>& buffer, Request request, Id id)
    {
        auto payload = build_request(buffer, request, id);
        auto n       = co_await async::write_exact(socket, payload);
        HANDLE_ERROR(n, payload.size(), "failed to send request payload");
        co_return Expect<void>{};
    }

    AExpect<void> send_response(
        Socket&               socket,
        Vec<u8>&              buffer,
        Procedure             proc,
        Var<Status, Response> response,
        Id                    id
    )
    {
        auto payload = build_response(buffer, proc, response, id);
        auto n       = co_await async::write_exact(socket, payload);
        HANDLE_ERROR(n, payload.size(), "failed to send response payload");
        co_return Expect<void>{};
    }

    AExpect<RequestHeader> receive_request_header(Socket& socket)
    {
        constexpr auto header_len = sizeof(Id) + sizeof(Procedure) + sizeof(u64);

        auto header = Array<u8, header_len>{};
        auto n      = co_await async::read_exact<u8>(socket, header);
        HANDLE_ERROR(n, header_len, "failed to read request header");

        auto reader = PayloadReader{ header };
        auto id     = reader.read_id().value();
        auto proc   = reader.read_procedure();    // can fail, invalid procedure
        auto size   = reader.read_int<u64>().value();

        if (not proc) {
            log_e("{}: received response for [{}] but it's an [invalid procedure]", __func__, id.inner());
            co_return Unexpect{ Status::bad_message };
        }

        co_return RequestHeader{ .id = id, .proc = *proc, .size = size };
    }

    AExpect<ResponseHeader> receive_response_header(Socket& socket)
    {
        constexpr auto header_len = sizeof(Id) + sizeof(Procedure) + sizeof(Status) + sizeof(u64);

        auto header = Array<u8, header_len>{};
        auto n      = co_await async::read_exact<u8>(socket, header);
        HANDLE_ERROR(n, header_len, "failed to read response header");

        auto reader = PayloadReader{ header };
        auto id     = reader.read_id().value();
        auto proc   = reader.read_procedure();    // can fail, invalid procedure
        auto status = reader.read_status().value();
        auto size   = reader.read_int<u64>().value();

        if (not proc) {
            log_e("{}: received response for [{}] but it's an [invalid procedure]", __func__, id.inner());
            co_return Unexpect{ Status::bad_message };
        }

        co_return ResponseHeader{ .id = id, .proc = *proc, .status = status, .size = size };
    }

    AExpect<Request> receive_request(Socket& socket, Vec<u8>& buffer, RequestHeader header)
    {
        buffer.resize(header.size);

        auto n1 = co_await async::read_exact<u8>(socket, buffer);
        HANDLE_ERROR(n1, buffer.size(), "failed to read request payload");

        auto req = parse_request(buffer, header.proc);
        co_return req ? Expect<Request>{ std::move(*req) } : Unexpect{ Status::bad_message };
    }

    AExpect<Response> receive_response(Socket& socket, Vec<u8>& buffer, ResponseHeader header)
    {
        if (header.status != Status{}) {
            co_return Unexpect{ header.status };
        }

        buffer.resize(header.size);

        auto n1 = co_await async::read_exact<u8>(socket, buffer);
        HANDLE_ERROR(n1, buffer.size(), "failed to read response payload");

        auto resp = parse_response(buffer, header.proc);
        co_return resp ? Expect<Response>{ *resp } : Unexpect{ Status::bad_message };
    }
}
