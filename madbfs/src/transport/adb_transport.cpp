#include "madbfs/transport/adb_transport.hpp"

#include "madbfs/adb.hpp"
#include "madbfs/cmd.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/slice.hpp>
#include <madbfs-common/util/split.hpp>

namespace
{
    /**
     * @brief Parse integral types from string.
     *
     * @tparam T The type to be parsed.
     *
     * @param str Input string.
     * @param base Integer base to use.
     *
     * @return Parsed integer on success else `std::nullopt`.
     */
    template <std::integral T>
    constexpr madbfs::Opt<T> parse_integral(madbfs::Str str, int base)
    {
        auto t         = T{};
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), t, base);
        if (ptr != str.data() + str.size() or ec != madbfs::Errc{}) {
            return {};
        }
        return t;
    }

    /**
     * @brief Strip directory and suffix from path.
     *
     * @param path Path string.
     *
     * @return Stripped path (aka filename only).
     */
    madbfs::Str get_basename(madbfs::Str path)
    {
        if (path != "/") {
            auto found = madbfs::sr::find(path | madbfs::sv::reverse, '/');
            if (found != path.rend()) {    // no '/' means already basename
                return madbfs::Str{ found.base(), path.end() };
            }
        }
        return path;
    }

    /**
     * @brief Parse date in the form "2025-06-04 12:09:05.450071907 +0700"
     *
     * @param date Date string.
     *
     * @return Time represented in `timespec`.
     */
    timespec parse_date(madbfs::Str date)
    {
        // apparently istringstream only have the overload for string_view from C++26 onwards. damn it!
        auto in = std::istringstream{ madbfs::String{ date } };    // unnecessary copy... :(
        auto tp = std::chrono::sys_time<std::chrono::nanoseconds>{};

        in >> std::chrono::parse("%F %T %z", tp);
        if (in.fail()) {
            madbfs::log_d(__func__, "fail to parse {:?}", date);
        }

        auto secs  = std::chrono::time_point_cast<std::chrono::seconds>(tp);
        auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(tp - secs);

        return { secs.time_since_epoch().count(), nsecs.count() };
    }

    /**
     * @brief Parse the output of `stat -c '%f|%h|%s|%u|%g|%x|%y|%z|%n' <path>`
     *
     * @paran str Input string.
     *
     * @return Parsed stat if success else `std::nullopt`.
     */
    madbfs::Opt<madbfs::Pair<madbfs::Str, madbfs::rpc::resp::Stat>> parse_file_stat(madbfs::Str str)
    {
        return madbfs::util::split_n<8>(str, '|').transform([](madbfs::util::SplitResult<8>&& res) {
            auto [mode_hex, hardlinks, size, uid, gid, atime, mtime, ctime] = res.result;
            return madbfs::Pair{
                get_basename(res.remainder),
                madbfs::rpc::resp::Stat{
                    .size  = parse_integral<off_t>(size, 10).value_or(0),
                    .links = parse_integral<nlink_t>(hardlinks, 10).value_or(0),
                    .mtime = parse_date(mtime),
                    .atime = parse_date(atime),
                    .ctime = parse_date(ctime),
                    .mode  = parse_integral<mode_t>(mode_hex, 16).value_or(0),
                    .uid   = parse_integral<uid_t>(uid, 10).value_or(0),
                    .gid   = parse_integral<uid_t>(gid, 10).value_or(0),
                },
            };
        });
    }

    /**
     * @brief Create a new quoted path.
     *
     * @param path Path to be quoted
     *
     * @return Quoted path.
     *
     *  NOTE: adb shell apparently needs double escaping
     */
    madbfs::String quote(madbfs::Str path)
    {
        return fmt::format("\"{}\"", path);
    }

    /**
     * @brief Check whether the device is connected through adb.
     */
    madbfs::AExpect<void> check_connection()
    {
        const auto* serial = ::getenv("ANDROID_SERIAL");
        if (serial == nullptr) [[unlikely]] {
            co_return std::unexpected{ std::errc::not_connected };
        }

        auto devices = co_await madbfs::adb::list_devices();
        if (not devices) {
            co_return std::unexpected{ std::errc::not_connected };
        }

        auto found = madbfs::sr::find(*devices, serial, &madbfs::adb::Device::serial);
        if (found == devices->end()) {
            co_return std::unexpected{ std::errc::not_connected };
        } else if (found->status != madbfs::adb::DeviceStatus::Device) {
            madbfs::log_d(__func__, "device connected but: {}", to_string(found->status));
            co_return std::unexpected{ std::errc::not_connected };
        }

        co_return madbfs::Expect<void>{};
    }
}

namespace madbfs
{
    class Handler
    {
    public:
        AExpect<rpc::Response> handle_req(rpc::req::Listdir req)
        {
            auto& [path, buf] = req;

            const auto qpath = quote(path);
            const auto cmd   = std::to_array<Str>({
                "adb",
                "shell",
                "find",
                qpath,
                "-maxdepth",
                "1",
                "-exec",
                "stat",
                "-c",
                "'%f|%h|%s|%u|%g|%x|%y|%z|%n'",
                "{}",
                "+",
            });

            auto res = co_await cmd::exec(cmd, "", false, false);
            if (not res.has_value()) {
                co_return Unexpect{ res.error() };
            }

            auto lines  = util::StringSplitter{ *res, '\n' };
            std::ignore = lines.next();    // ignore first line, the directory itself

            buf.clear();

            auto slices = Vec<Pair<util::Slice, rpc::resp::Stat>>{};

            while (auto line = lines.next()) {
                auto parsed = parse_file_stat(util::strip(*line));
                if (not parsed) {
                    continue;
                }

                auto [name, stat] = std::move(*parsed);

                auto name_u8 = reinterpret_cast<const u8*>(name.data());
                auto off     = buf.size();

                buf.insert(buf.end(), name_u8, name_u8 + name.size());
                auto slice = util::Slice{ off, name.size() };

                slices.emplace_back(std::move(slice), std::move(stat));
            }

            auto entries = Vec<Pair<Str, rpc::resp::Stat>>{};
            entries.reserve(slices.size());

            for (auto&& [slice, stat] : slices) {
                auto name = Str{ reinterpret_cast<const char*>(buf.data()) + slice.offset, slice.size };
                entries.emplace_back(std::move(name), std::move(stat));
            }

            co_return rpc::resp::Listdir{ .entries = std::move(entries) };
        }

        AExpect<rpc::Response> handle_req(rpc::req::Stat req)
        {
            auto res = co_await cmd::exec(
                { "adb", "shell", "stat", "-c", "'%f|%h|%s|%u|%g|%x|%y|%z|%n'", quote(req.path) }
            );

            co_return res.and_then([&](Str out) {
                return ok_or(parse_file_stat(util::strip(out)), Errc::io_error)
                    .transform([](auto parsed) { return std::get<1>(parsed); })
                    .transform_error([&](auto err) {
                        log_e(__func__, "parsing stat failed [{}]", req.path);
                        return err;
                    });
            });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Readlink req)
        {
            auto& [path, buf] = req;

            auto res = co_await cmd::exec({ "adb", "shell", "readlink", quote(path) });
            buf.clear();

            co_return res.transform([&](Str target) {
                auto stripped = util::strip(target);
                buf.insert(buf.end(), stripped.begin(), stripped.end());
                return rpc::resp::Readlink{
                    .target = Str{ reinterpret_cast<const char*>(buf.data()), stripped.size() },
                };
            });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Mknod req)
        {
            auto res = co_await cmd::exec({ "adb", "shell", "touch", quote(req.path) });
            co_return res.transform([](auto&&) { return rpc::resp::Mknod{}; });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Mkdir req)
        {
            auto res = co_await cmd::exec({ "adb", "shell", "mkdir", quote(req.path) });
            co_return res.transform([](auto&&) { return rpc::resp::Mkdir{}; });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Unlink req)
        {
            auto res = co_await cmd::exec({ "adb", "shell", "rm", quote(req.path) });
            co_return res.transform([](auto&&) { return rpc::resp::Unlink{}; });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Rmdir req)
        {
            auto res = co_await cmd::exec({ "adb", "shell", "rmdir", quote(req.path) });
            co_return res.transform([](auto&&) { return rpc::resp::Rmdir{}; });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Rename req)
        {
            if (req.flags == RENAME_EXCHANGE) {
                // NOTE: there is no --exchange flag on Android's mv
                // NOTE: renameat2 returns EINVAL when the fs doesn't support exchange operation see man
                // rename(2)
                co_return Unexpect{ Errc::invalid_argument };
            } else if (req.flags == RENAME_NOREPLACE) {
                auto res = co_await cmd::exec({ "adb", "shell", "mv", "-n", quote(req.from), quote(req.to) });
                co_return res.transform([](auto&&) { return rpc::resp::Rename{}; });
            } else {
                auto res = co_await cmd::exec({ "adb", "shell", "mv", quote(req.from), quote(req.to) });
                co_return res.transform([](auto&&) { return rpc::resp::Rename{}; });
            }
        }

        AExpect<rpc::Response> handle_req(rpc::req::Truncate req)
        {
            const auto size_str = fmt::format("{}", req.size);

            auto res = co_await cmd::exec({ "adb", "shell", "truncate", "-s", size_str, quote(req.path) });
            co_return res.transform([](auto&&) { return rpc::resp::Truncate{}; });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Utimens req)
        {
            namespace chr = std::chrono;

            static const chr::time_zone* z = nullptr;

            if (z == nullptr) {
                try {
                    z = chr::current_zone();
                } catch (const std::runtime_error& e) {
                    log_c(__func__, "{}", e.what());
                    co_return Unexpect{ Errc::resource_unavailable_try_again };
                }
            }

            for (auto [time, flag] : { Pair{ req.atime, "-a" }, Pair{ req.mtime, "-m" } }) {
                switch (time.tv_nsec) {
                case UTIME_OMIT: continue;
                case UTIME_NOW: {
                    auto res = co_await cmd::exec({ "adb", "shell", "touch", "-c", flag, quote(req.path) });
                    if (not res) {
                        co_return Unexpect{ res.error() };
                    }
                } break;
                default: {
                    const auto tp = chr::system_clock::time_point{ chr::seconds{ time.tv_sec } };
                    const auto zt = z->to_local(chr::floor<chr::seconds>(tp));
                    const auto t  = std::format("{:%Y%m%d%H%M.%S}{}", zt, time.tv_nsec);    // fmt can't

                    log_i(__func__, "utimens to {}", t);

                    auto res = co_await cmd::exec(
                        { "adb", "shell", "touch", "-c", flag, "-t", t, quote(req.path) }
                    );
                    if (not res) {
                        co_return Unexpect{ res.error() };
                    }
                }
                }
            }

            co_return rpc::resp::Utimens{};
        }

        AExpect<rpc::Response> handle_req(rpc::req::CopyFileRange req)
        {
            const auto skip  = fmt::format("skip={}", req.in_offset);
            const auto count = fmt::format("count={}", req.size);
            const auto ifile = fmt::format("if=\"{}\"", req.in_path);
            const auto seek  = fmt::format("seek={}", req.out_offset);
            const auto ofile = fmt::format("of=\"{}\"", req.out_path);

            // count_bytes: https://stackoverflow.com/a/40792605/16506263
            // notrunc    : https://unix.stackexchange.com/a/146923
            const auto cmd = std::to_array<Str>({
                "adb",
                "shell",
                "dd",
                "iflag=skip_bytes,count_bytes",
                skip,
                count,
                ifile,
                "oflag=seek_bytes",
                "conv=notrunc",
                seek,
                ofile,
            });

            auto res = co_await cmd::exec(cmd, "", true, true);

            // example output
            /*
             * conv=4
             * 1024+0 records in
             * 1024+0 records out
             * 1048576 bytes (1.0 M) copied, 0.054401 s, 18 M/s
             */

            co_return res.transform([&](Str str) {
                log_d(__func__, "copy_file_range: {:?}", str);
                return util::split_n<4>(str, '\n')
                    .and_then([](util::SplitResult<4> r) { return util::split_n<1>(r.result[3], ' '); })
                    .and_then([](util::SplitResult<1> r) { return parse_integral<usize>(r.result[0], 10); })
                    .transform([](auto n) { return rpc::resp::CopyFileRange{ .size = n }; })
                    .value_or(rpc::resp::CopyFileRange{ .size = 0 });
            });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Open req)
        {
            auto fd = ++m_fd_counter;
            m_fd_map.emplace(fd, req.path);
            co_return rpc::resp::Open{ .fd = fd };
        }

        AExpect<rpc::Response> handle_req(rpc::req::Close req)
        {
            auto entry = m_fd_map.find(req.fd);
            if (entry == m_fd_map.end()) {
                co_return Unexpect{ Errc::bad_file_descriptor };
            }

            m_fd_map.erase(entry);
            co_return rpc::resp::Close{};
        }

        AExpect<rpc::Response> handle_req(rpc::req::Read req)
        {
            auto& [fd, offset, out] = req;

            auto entry = m_fd_map.find(fd);
            if (entry == m_fd_map.end()) {
                co_return Unexpect{ Errc::bad_file_descriptor };
            }

            const auto& path  = entry->second;
            const auto  skip  = fmt::format("skip={}", offset);
            const auto  count = fmt::format("count={}", out.size());
            const auto  ifile = fmt::format("if=\"{}\"", path);

            // `bs` is skipped, relies on `count_bytes`: https://stackoverflow.com/a/40792605/16506263
            auto res = co_await cmd::exec(
                { "adb", "shell", "dd", "iflag=skip_bytes,count_bytes", skip, count, ifile }
            );

            co_return res.transform([&](Str str) {
                auto size = std::min(str.size(), out.size());
                std::copy_n(str.begin(), size, out.begin());
                return rpc::resp::Read{ .read = out };
            });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Write req)
        {
            auto entry = m_fd_map.find(req.fd);
            if (entry == m_fd_map.end()) {
                co_return Unexpect{ Errc::bad_file_descriptor };
            }

            const auto& path  = entry->second;
            const auto  seek  = fmt::format("seek={}", req.offset);
            const auto  ofile = fmt::format("of=\"{}\"", path);

            auto in_str = Str{ reinterpret_cast<const char*>(req.in.data()), req.in.size() };

            // `notrunc` flag is necessary to prevent truncating file: https://unix.stackexchange.com/a/146923
            auto res = co_await cmd::exec(
                { "adb", "shell", "dd", "oflag=seek_bytes", "conv=notrunc", seek, ofile }, in_str
            );

            // assume all the data is written to device on success
            co_return res.transform([&](auto&&) { return rpc::resp::Write{ .size = in_str.size() }; });
        }

        AExpect<rpc::Response> handle_req(rpc::req::Ping req)
        {
            auto res = co_await check_connection();
            co_return res.transform([&] { return rpc::resp::Ping{ .num = req.num }; });
        }

    private:
        using FdMap = std::unordered_map<u64, String>;

        u64   m_fd_counter = 0;
        FdMap m_fd_map;
    };
}

namespace madbfs::transport
{
    AdbTransport::~AdbTransport()
    {
        stop(Errc::operation_canceled);
    }

    AExpect<Uniq<AdbTransport>> AdbTransport::create()
    {
        if (auto res = co_await check_connection(); not res) {
            co_return Unexpect{ res.error() };
        }
        co_return Uniq<AdbTransport>{ new AdbTransport{ co_await async::current_executor() } };
    }

    void AdbTransport::stop(rpc::Status status)
    {
        if (m_running) {
            m_running = false;

            for (auto& [id, promise] : m_requests) {
                promise.result.set_value(Unexpect{ status });
            }
            m_requests.clear();

            m_in_channel.cancel();
            m_in_channel.close();
            m_out_channel.cancel();
            m_out_channel.close();
        }

        m_pool.stop();
        m_pool.wait();
    }

    Await<void> AdbTransport::start()
    {
        log_d(__func__, "called");

        m_running = true;
        auto exec = co_await async::current_executor();

        async::spawn(exec, response_receive(), [&](std::exception_ptr e, Expect<void> res) {
            m_running = false;

            log::log_exception(e, "response_receive");
            if (not res) {
                log_w("response_receive", "finished with error: {}", err_msg(res.error()));
            }

            if (not m_requests.empty()) {
                log_e("response_receive", "there are {} promises unhandled", m_requests.size());
                for (auto& [id, p] : m_requests) {
                    p.result.set_value(Unexpect{ e ? Errc::state_not_recoverable : Errc::operation_canceled }
                    );
                }
            }

            m_requests.clear();
            m_in_channel.cancel();
            m_in_channel.reset();
            m_out_channel.cancel();
            m_out_channel.reset();
        });

        async::spawn(exec, request_dispatch(), [&](std::exception_ptr e, Expect<void> res) {
            log::log_exception(e, "request_dispatch");
            if (not res) {
                log_e("request_dispatch", "finished with error: {}", err_msg(res.error()));
            }
            log_e("request_dispatch", "stopped sending requests");
        });
    }

    AExpect<rpc::Response> AdbTransport::send(rpc::Request req)
    {
        if (not m_running) {
            co_return Unexpect{ Errc::resource_unavailable_try_again };
        }

        auto id      = next_id();
        auto promise = saf::promise<Expect<rpc::Response>>{ co_await async::current_executor() };
        auto future  = promise.get_future();

        auto [_, ok] = m_requests.try_emplace(id, req.proc(), std::move(promise));
        assert(ok and "id is always incremented, insertion should always happens");

        if (auto res = co_await m_in_channel.async_send({}, { id, req }); not res) {
            log_e(__func__, "failed to send payload to channel: {}", res.error().message());
            co_return Unexpect{ async::to_generic_err(res.error(), Errc::broken_pipe) };
        }

        log_d(__func__, "REQ QUEUED {} [{}]", id.inner(), rpc::to_string(req));

        co_return co_await future.async_extract();
    }

    AExpect<rpc::Response> AdbTransport::send(rpc::Request req, Milliseconds timeout)
    {
        if (not m_running) {
            co_return Unexpect{ Errc::resource_unavailable_try_again };
        }

        auto id      = next_id();
        auto promise = saf::promise<Expect<rpc::Response>>{ co_await async::current_executor() };
        auto future  = promise.get_future();

        auto [_, ok] = m_requests.try_emplace(id, req.proc(), std::move(promise));
        assert(ok and "id is always incremented, insertion should always happens");

        if (auto res = co_await m_in_channel.async_send({}, { id, req }); not res) {
            log_e(__func__, "failed to send payload to channel: {}", res.error().message());
            co_return Unexpect{ async::to_generic_err(res.error(), Errc::broken_pipe) };
        }

        log_d(__func__, "REQ QUEUED {} [{}]", id.inner(), rpc::to_string(req));

        co_await async::timeout(future.async_wait(async::use_awaitable), timeout, [&] {
            if (auto entry = m_requests.extract(id); not entry.empty()) {
                log_d("send", "REQ CANCELLED {} [{}]", id.inner(), rpc::to_string(req));
                entry.mapped().result.set_value(Unexpect{ Errc::timed_out });
            }
        });

        co_return future.is_ready() ? future.extract() : Unexpect{ Errc::timed_out };
    }

    AExpect<void> AdbTransport::request_dispatch()
    {
        auto handler = Handler{};

        while (m_running and m_in_channel.is_open()) {
            auto id_req = co_await m_in_channel.async_receive();
            if (not id_req) {
                log_e(__func__, "failed to recv payload from channel: {}", id_req.error().message());
                co_return Unexpect{ async::to_generic_err(id_req.error(), Errc::broken_pipe) };
            }

            auto [id, req] = std::move(*id_req);
            auto promise   = m_requests.find(id);
            if (promise == m_requests.end()) {
                log_e(__func__, "request {} has no associated promise ", id.inner());
                continue;
            }

            async::spawn(
                m_pool,
                req.visit([&]<rpc::IsRequest R>(R& req) { return handler.handle_req(req); }),
                [&, id](std::exception_ptr e, Expect<rpc::Response> resp) {
                    log::log_exception(e, "handler");
                    async::spawn(
                        m_out_channel.get_executor(),
                        m_out_channel.async_send({}, { id, std::move(resp) }),
                        [](std::exception_ptr e, Expect<void, net::error_code> res) {
                            log::log_exception(e, "handler");
                            if (not res) {
                                log_e("handler", "finished with error: {}", res.error().message());
                            }
                        }
                    );
                }
            );
        }

        m_pool.wait();
        log_d(__func__, "listening complete");

        co_return Expect<void>{};
    }

    AExpect<void> AdbTransport::response_receive()
    {
        while (m_running and m_out_channel.is_open()) {
            auto id_resp = co_await m_out_channel.async_receive();
            if (not id_resp) {
                log_e(__func__, "failed to receive response from channel: {}", id_resp.error().message());
                co_return Unexpect{ async::to_generic_err(id_resp.error(), Errc::broken_pipe) };
            }

            auto [id, response] = std::move(*id_resp);

            auto req = m_requests.extract(id);
            if (req.empty()) {
                log_e(__func__, "response incoming for id {} but no promise", id.inner());
                continue;
            }

            auto& [proc, res] = req.mapped();
            log_d(__func__, "RESP RECV {} [{}]", id.inner(), rpc::to_string(proc));

            res.set_value(std::move(response));
        }

        co_return Expect<void>{};
    }
}
