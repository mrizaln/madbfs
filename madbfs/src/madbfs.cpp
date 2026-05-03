#include "madbfs/madbfs.hpp"

#include <madbfs-common/log.hpp>

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <boost/json.hpp>

namespace json = boost::json;

namespace madbfs
{
    constexpr usize lowest_page_size  = 64 * 1024;
    constexpr usize highest_page_size = 4 * 1024 * 1024;
    constexpr usize lowest_max_pages  = 128;

    // NOTE: normally, I'll use overloads of coro-lambdas with `this auto` as 1st param, but gcc segfaulted.
    // see: https://gcc.gnu.org/bugzilla/show_bug.cgi?format=multiple&id=114632.
    // last-checked: 2026-02-13 21:15.
    // on: gcc (GCC) 15.2.1 20251211 (Red Hat 15.2.1-5).
    struct Madbfs::IpcHandler
    {
        Await<json::value> handle(ipc::op::Info)
        {
            const auto page_size     = madbfs.fs().cache().page_size();
            const auto max_pages     = madbfs.fs().cache().max_pages();
            const auto current_pages = madbfs.fs().cache().current_pages();
            const auto ttl_sec       = madbfs.fs().ttl().transform(&Seconds::count);
            const auto timeout_sec   = madbfs.m_timeout.transform(&Seconds::count);

            co_return json::value{
                { "connection", madbfs.m_connection.name() },
                { "log_level", log::level_to_str(log::get_level()) },
                { "ttl", ttl_sec.value_or(0) },
                { "timeout", timeout_sec.value_or(0) },
                { "page_size", page_size / 1024 },
                { "cache_size",
                  { { "max", page_size * max_pages / 1024 / 1024 },
                    { "current", page_size * current_pages / 1024 / 1024 } } },
            };
        }

        Await<json::value> handle(ipc::op::InvalidateCache)
        {
            const auto page_size     = madbfs.fs().cache().page_size();
            const auto current_pages = madbfs.fs().cache().current_pages();

            co_await madbfs.fs().cache().invalidate_all();

            co_return json::value{ { "size", page_size * current_pages / 1024 / 1024 } };
        }

        Await<json::value> handle(ipc::op::ExpireStat)
        {
            auto count = madbfs.fs().expires_all();
            co_return json::value{ { "count", count } };
        }

        Await<json::value> handle(ipc::op::SetPageSize size)
        {
            const auto old_size = madbfs.fs().cache().page_size();
            const auto old_max  = madbfs.fs().cache().max_pages();

            auto new_size = std::bit_ceil(size.kib * 1024);
            new_size      = std::clamp(new_size, lowest_page_size, highest_page_size);

            auto new_max = std::bit_ceil(old_max * old_size / new_size);
            new_max      = std::max(new_max, lowest_max_pages);

            co_await madbfs.fs().cache().set_page_size(new_size);
            co_await madbfs.fs().cache().set_max_pages(new_max);

            co_return json::value{
                { "page_size",
                  { { "old", old_size / 1024 },    //
                    { "new", new_size / 1024 } } },
                { "cache_size",
                  { { "old", old_max * old_size / 1024 / 1024 },
                    { "new", new_max * new_size / 1024 / 1024 } } },
            };
        }

        Await<json::value> handle(ipc::op::SetCacheSize size)
        {
            const auto page    = madbfs.fs().cache().page_size();
            const auto old_max = madbfs.fs().cache().max_pages();

            auto new_max = std::bit_ceil(size.mib * 1024 * 1024 / page);
            new_max      = std::max(new_max, lowest_max_pages);

            co_await madbfs.fs().cache().set_max_pages(new_max);

            co_return json::value{
                { "cache_size",
                  { { "old", old_max * page / 1024 / 1024 },    //
                    { "new", new_max * page / 1024 / 1024 } } },
            };
        }

        Await<json::value> handle(ipc::op::SetTTL ttl)
        {
            const auto new_ttl = ttl.sec < 1 ? std::nullopt : Opt<Seconds>{ ttl.sec };
            const auto old_ttl = madbfs.fs().set_ttl(new_ttl);

            co_return json::value{
                { "ttl",
                  { { "old", old_ttl.transform(&Seconds::count).value_or(0) },    //
                    { "new", new_ttl.transform(&Seconds::count).value_or(0) } } },
            };
        }

        Await<json::value> handle(ipc::op::SetTimeout ttl)
        {
            auto old = std::exchange(madbfs.m_timeout, ttl.sec < 1 ? std::nullopt : Opt<Seconds>{ ttl.sec });
            co_return json::value{
                { "timeout",
                  { { "old", old.transform(&Seconds::count).value_or(0) },    //
                    { "new", madbfs.m_timeout.transform(&Seconds::count).value_or(0) } } },
            };
        }

        Await<json::value> handle(ipc::op::SetLogLevel op)
        {
            const auto prev_level = log::get_level();
            const auto new_level  = log::level_from_str(op.lvl).value_or(prev_level);

            log::set_level(new_level);

            co_return json::value{
                { "log_level",
                  { { "old", log::level_to_str(prev_level) },    //
                    { "new", log::level_to_str(new_level) } } },
            };
        }

        Await<json::value> handle(ipc::op::Unmount)
        {
            ::fuse_exit(madbfs.m_fuse);
            co_return json::value{ nullptr };
        }

        Madbfs& madbfs;
    };
}

namespace madbfs
{
    Connection Madbfs::prepare_connection(async::Context& ctx, args::Connection connection)
    {
        return connection.visit(Overload{
            [&](args::connection::AdbOnly) {
                return Connection{ ctx, connection_strategy::Adb{} };    //
            },
            [&](args::connection::NoServer c) {
                return Connection{ ctx, connection_strategy::Proxy{ std::nullopt, c.port } };
            },
            [&](args::connection::Server c) {
                auto path = path::create(c.path.c_str()).transform([](auto p) { return p.path.owned(); });
                return Connection{ ctx, connection_strategy::Proxy{ path, c.port } };
            },
        });
    }

    Opt<ipc::Server> Madbfs::create_ipc(async::Context& ctx)
    {
        auto socket_path = [] -> String {
            const auto* res = std::getenv("XDG_RUNTIME_DIR");
            return res ? res : "/tmp";
        }();

        const auto* serial = std::getenv("ANDROID_SERIAL");
        if (serial == nullptr) {
            return {};
        }

        socket_path += '/';
        socket_path += "madbfs@";
        socket_path += serial;
        socket_path += ".sock";

        auto ipc = ipc::Server::create(ctx, socket_path);
        if (not ipc.has_value()) {
            log_e(__func__, "failed to initialize ipc: {}", err_msg(ipc.error()));
            return {};
        }

        log_i(__func__, "succesfully created ipc: {}", ipc->path());
        return std::move(*ipc);
    }

    void Madbfs::work_thread_function(async::Context& ctx)
    {
        try {
            log_i(__func__, "io_context running...");
            auto num_handlers = ctx.run();
            log_i(__func__, "io_context stopped with {} handlers executed", num_handlers);
        } catch (const std::exception& e) {
            log_w(__func__, "io_context stopped with an exception: {}", e.what());
        } catch (...) {
            log_w(__func__, "io_context stopped with an exception (unknown type)");
        }
    }

    Madbfs::Madbfs(
        struct fuse*     fuse,
        args::Connection connection,
        usize            page_size,
        usize            max_pages,
        Str              mountpoint,
        Opt<Seconds>     ttl,
        Opt<Seconds>     timeout
    )
        : m_fuse{ fuse }
        , m_async_ctx{}
        , m_work_guard{ m_async_ctx.get_executor() }
        , m_work_thread{ [this] { work_thread_function(m_async_ctx); } }
        , m_connection{ prepare_connection(m_async_ctx, connection) }
        , m_fs{ m_connection, page_size, max_pages, ttl }
        , m_ipc{ create_ipc(m_async_ctx) }
        , m_watchdog_timer{ m_async_ctx }
        , m_reaper_timer{ m_async_ctx }
        , m_signal{ m_async_ctx, SIGINT, SIGTERM }
        , m_mountpoint{ mountpoint }
        , m_timeout{ timeout }
    {
        if (m_ipc) {
            auto coro = m_ipc->launch([this](ipc::FsOp op) { return ipc_handler(op); });
            async::spawn(m_async_ctx, std::move(coro), [](std::exception_ptr e) {
                log::log_exception(e, "Madbfs");
            });
        }

        async::spawn(m_async_ctx, watchdog(), [](std::exception_ptr e) { log::log_exception(e, "Madbfs"); });
        async::spawn(m_async_ctx, reaper(), [](std::exception_ptr e) { log::log_exception(e, "Madbfs"); });

        async::spawn(m_async_ctx, m_connection.start(), [](std::exception_ptr e) {
            log::log_exception(e, "Madbfs");
        });

        m_signal.async_wait([this, pid = ::getpid()](net::error_code ec, int sig) {
            if (not ec) {
                assert(m_fuse != nullptr);
                madbfs::log_w("Madbfs", "signal raised: SIG{} ({})", ::sigabbrev_np(sig), sig);
                ::fuse_exit(m_fuse);
                ::kill(pid, SIGPIPE);
            }
        });
    }

    Madbfs::~Madbfs()
    {
        m_signal.cancel();

        if (m_ipc) {
            m_ipc->stop();
        }

        m_watchdog_timer.cancel();
        m_reaper_timer.cancel();

        async::block(m_async_ctx, m_fs.shutdown());
        m_connection.cancel(Errc::operation_canceled);

        m_work_guard.reset();
        m_async_ctx.stop();
        m_work_thread.join();
    }

    Await<json::value> Madbfs::ipc_handler(ipc::FsOp op)
    {
        auto handler = IpcHandler{ *this };
        co_return co_await op.visit([&](auto&& op) { return handler.handle(op); });
    }

    Await<void> Madbfs::watchdog()
    {
        using namespace std::chrono_literals;
        constexpr auto interval = 10s;

        while (true) {
            m_watchdog_timer.expires_after(interval);
            if (auto res = co_await m_watchdog_timer.async_wait(); not res) {
                break;
            }

            log_d(__func__, "checking connection");
            auto ok = co_await m_connection.ping(m_timeout.value_or(Seconds::max()));

            if (not ok) {
                log_i(__func__, "connection is timed out");
                if (auto res = co_await m_connection.reconnect(); res) {
                    co_await m_fs.cache().invalidate_fds(false);
                    co_await m_connection.start();
                }
            } else if (not m_connection.is_optimal()) {
                log_i(__func__, "connection is ok but not optimized. trying to optimize...");
                if (auto res = co_await m_connection.optimize(); res) {
                    co_await m_fs.cache().invalidate_fds(false);
                    co_await m_connection.start();
                }
            } else {
                log_d(__func__, "connection is ok");
            }
        }
    }

    Await<void> Madbfs::reaper()
    {
        using namespace std::chrono_literals;
        constexpr auto interval = 10s;

        while (true) {
            m_reaper_timer.expires_after(interval);
            if (auto res = co_await m_reaper_timer.async_wait(); not res) {
                break;
            }

            co_await m_fs.cache().clean_stale_fds();

            const auto& handles = m_fs.handles();

            auto cap   = handles.size();
            auto pred  = [](auto&& v) { return std::get<0>(v) == nullptr; };
            auto empty = static_cast<usize>(sr::count_if(handles.iter(), pred));

            log_w(__func__, "file handles [cap={:>40d}|open={:>40d}|null={:>40d}]", cap, cap - empty, empty);
        }
    }
}
