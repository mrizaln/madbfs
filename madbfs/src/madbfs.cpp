#include "madbfs/madbfs.hpp"

#include "madbfs/bridge.hpp"
#include "madbfs/operations_ll.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/defer.hpp>

#include <boost/json.hpp>

using namespace madbfs;

namespace json = boost::json;

// madbfs.hpp impl: Madbfs::IpcHandler
namespace madbfs
{
    constexpr usize lowest_page_size  = 64 * 1024;
    constexpr usize highest_page_size = 4 * 1024 * 1024;
    constexpr usize lowest_max_pages  = 128;

    // NOTE: normally, I'd use overloads of coro-lambdas with `this auto` as 1st param, but gcc segfaulted.
    // see: https://gcc.gnu.org/bugzilla/show_bug.cgi?format=multiple&id=114632.
    // last-checked: 2026-02-13 21:15.
    // on: gcc (GCC) 15.2.1 20251211 (Red Hat 15.2.1-5).
    struct Madbfs::IpcHandler
    {
        AExpect<json::value> handle(ipc::op::Info)
        {
            const auto& cache = madbfs.fs().cache();

            const auto ttl_sec     = madbfs.m_ttl.transform(&Seconds::count).value_or(0);
            const auto timeout_sec = madbfs.m_timeout.transform(&Seconds::count).value_or(0);

            if (cache) {
                const auto page_size     = cache->page_size();
                const auto max_pages     = cache->max_pages();
                const auto current_pages = cache->current_pages();

                co_return json::value{
                    { "serial", std::getenv("ANDROID_SERIAL") },
                    { "transport", madbfs.m_conn.name() },
                    { "root", madbfs.m_root.str() },
                    { "log_level", log::level_to_str(log::get_level()) },
                    { "ttl", ttl_sec },
                    { "timeout", timeout_sec },
                    { "cache",
                      { { "page_size", page_size / 1024 },
                        { "cache_size",
                          { { "max", page_size * max_pages / 1024 / 1024 },
                            { "current", page_size * current_pages / 1024 / 1024 } } } } }
                };
            } else {
                co_return json::value{
                    { "serial", std::getenv("ANDROID_SERIAL") },
                    { "transport", madbfs.m_conn.name() },
                    { "root", madbfs.m_root.str() },
                    { "log_level", log::level_to_str(log::get_level()) },
                    { "ttl", ttl_sec },
                    { "timeout", timeout_sec },
                    { "cache", nullptr },
                };
            }
        }

        AExpect<json::value> handle(ipc::op::InvalidateCache)
        {
            auto cache = madbfs.fs().cache();
            if (not cache) {
                co_return Unexpect{ Errc::operation_not_supported };
            }

            const auto page_size     = cache->page_size();
            const auto current_pages = cache->current_pages();

            co_await cache->invalidate_all();

            co_return json::value{ { "size", page_size * current_pages / 1024 / 1024 } };
        }

        AExpect<json::value> handle(ipc::op::ExpireStat)
        {
            auto& fs    = madbfs.fs();
            auto  count = fs.walk(fs.tree().root(), [&](tree::Entry entry) {
                auto& node   = entry.node;
                auto  name   = node.name();
                auto  parent = node.parent().to_u64();
                fuse_lowlevel_notify_inval_entry(madbfs.m_fuse, parent, name.data(), name.size());
            });
            co_return json::value{ { "count", count } };
        }

        AExpect<json::value> handle(ipc::op::SetPageSize size)
        {
            auto cache = madbfs.fs().cache();
            if (not cache) {
                co_return Unexpect{ Errc::operation_not_supported };
            }

            const auto old_size = cache->page_size();
            const auto old_max  = cache->max_pages();

            auto new_size = std::bit_ceil(size.kib * 1024);
            new_size      = std::clamp(new_size, lowest_page_size, highest_page_size);

            auto new_max = std::bit_ceil(old_max * old_size / new_size);
            new_max      = std::max(new_max, lowest_max_pages);

            co_await cache->set_page_size(new_size);
            co_await cache->set_max_pages(new_max);

            co_return json::value{
                { "page_size",
                  { { "old", old_size / 1024 },    //
                    { "new", new_size / 1024 } } },
                { "cache_size",
                  { { "old", old_max * old_size / 1024 / 1024 },
                    { "new", new_max * new_size / 1024 / 1024 } } },
            };
        }

        AExpect<json::value> handle(ipc::op::SetCacheSize size)
        {
            auto cache = madbfs.fs().cache();
            if (not cache) {
                co_return Unexpect{ Errc::operation_not_supported };
            }

            const auto page    = cache->page_size();
            const auto old_max = cache->max_pages();

            auto new_max = std::bit_ceil(size.mib * 1024 * 1024 / std::max(page, 1uz));
            new_max      = std::max(new_max, lowest_max_pages);

            if (cache) {
                co_await cache->set_max_pages(new_max);
            } else {
                new_max = 0;
            }

            co_return json::value{
                { "cache_size",
                  { { "old", old_max * page / 1024 / 1024 },    //
                    { "new", new_max * page / 1024 / 1024 } } },
            };
        }

        AExpect<json::value> handle(ipc::op::SetTTL ttl)
        {
            const auto new_ttl = ttl.sec < 1 ? std::nullopt : Opt<Seconds>{ ttl.sec };
            const auto old_ttl = std::exchange(madbfs.m_ttl, new_ttl);

            co_return json::value{
                { "ttl",
                  { { "old", old_ttl.transform(&Seconds::count).value_or(0) },    //
                    { "new", new_ttl.transform(&Seconds::count).value_or(0) } } },
            };
        }

        AExpect<json::value> handle(ipc::op::SetTimeout ttl)
        {
            auto old = std::exchange(madbfs.m_timeout, ttl.sec < 1 ? std::nullopt : Opt<Seconds>{ ttl.sec });
            co_return json::value{
                { "timeout",
                  { { "old", old.transform(&Seconds::count).value_or(0) },    //
                    { "new", madbfs.m_timeout.transform(&Seconds::count).value_or(0) } } },
            };
        }

        AExpect<json::value> handle(ipc::op::SetLogLevel op)
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

        AExpect<json::value> handle(ipc::op::Unmount)
        {
            ::fuse_session_exit(madbfs.m_fuse);
            co_return json::value{ nullptr };
        }

        Madbfs& madbfs;
    };
}

// madbfs.hpp impl: Madbfs
namespace madbfs
{
    Connection Madbfs::prepare_connection(async::Context& ctx, args::Connection connection)
    {
        namespace conn = connection_strategy;
        return connection.visit(Overload{
            [&](args::AdbOnly) { return Connection{ ctx, conn::Adb{} }; },
            [&](args::NoServer c) { return Connection{ ctx, conn::Proxy{ std::nullopt, c.port } }; },
            [&](args::Server c) { return Connection{ ctx, conn::Proxy{ c.abi, c.port } }; },
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
        fuse_session*   se,
        async::Context& ctx,
        Connection&     conn,
        Filesystem&     fs,
        path::Path      custom_root,
        Str             mount_point,
        Opt<Seconds>    ttl,
        Opt<Seconds>    timeout
    )
        : m_fuse{ se }
        , m_conn{ conn }
        , m_fs{ fs }
        , m_ipc{ create_ipc(ctx) }
        , m_watchdog_timer{ ctx }
        , m_reaper_timer{ ctx }
        , m_signal{ ctx, SIGINT, SIGTERM }
        , m_root{ custom_root.owned() }
        , m_mountpoint{ mount_point }
        , m_ttl{ ttl }
        , m_timeout{ timeout }
    {
    }

    AExpect<void> Madbfs::start()
    {
        auto exec = co_await async::current_executor();

        // async::spawn(exec, watchdog(), [](std::exception_ptr e) { log::log_exception(e, "start"); });
        // async::spawn(exec, reaper(), [](std::exception_ptr e) { log::log_exception(e, "start"); });

        m_signal.async_wait([this, pid = ::getpid()](net::error_code ec, int sig) {
            if (not ec) {
                assert(m_fuse != nullptr);
                log_w("Madbfs", "signal raised: SIG{} ({})", ::sigabbrev_np(sig), sig);
                ::fuse_session_exit(m_fuse);
                ::kill(pid, SIGPIPE);
            }
        });

        if (m_ipc) {
            auto handler = [h = IpcHandler{ *this }](ipc::FsOp op) mutable {
                return op.visit([&](auto&& op) { return h.handle(op); });
            };
            async::spawn(exec, m_ipc->launch(std::move(handler)), [](std::exception_ptr e) {
                log::log_exception(e, "start");
            });
        }

        m_started = true;
        co_return Expect<void>{};
    }

    void Madbfs::stop()
    {
        if (not m_started) {
            return;
        }

        m_signal.cancel();

        if (m_ipc) {
            m_ipc->stop();
            m_ipc.reset();
        }

        m_watchdog_timer.cancel();
        m_reaper_timer.cancel();
    }

    Madbfs::~Madbfs()
    {
        stop();
    }

    Expect<path::Path> Madbfs::create_path(const char* path)
    {
        thread_local static auto string = String{};
        thread_local static auto comps  = Vec<util::Slice>{};

        if (m_root.is_root()) {
            return ok_or(path::create_with(comps, path), Errc::invalid_argument);
        }

        string.clear();
        string.assign(m_root.str());
        string.ends_with('/') ? void() : string.push_back('/');
        string.append(path);

        return ok_or(path::create_with(comps, string), Errc::invalid_argument);
    }

    Expect<Array<path::Path, 2>> Madbfs::create_path2(const char* path1, const char* path2)
    {
        thread_local static auto strings = Array<String, 2>{};
        thread_local static auto compss  = Array<Vec<util::Slice>, 2>{};

        auto paths  = Array{ path1, path2 };
        auto result = Array<path::Path, 2>{};

        for (auto i : sv::iota(0uz, 2uz)) {
            if (m_root.is_root()) {
                auto path = path::create_with(compss[i], paths[i]);
                if (not path) {
                    return Unexpect{ Errc::invalid_argument };
                }
                result[i] = std::move(*path);
                continue;
            }

            strings[i].clear();
            strings[i].assign(m_root.str());
            strings[i].ends_with('/') ? void() : strings[i].push_back('/');
            strings[i].append(paths[i]);

            auto path = path::create_with(compss[i], strings[i]);
            if (not path) {
                return Unexpect{ Errc::invalid_argument };
            }
            result[i] = std::move(*path);
        }

        return result;
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
            auto ok = co_await m_conn.ping(m_timeout);

            if (not ok) {
                log_i(__func__, "connection is timed out");
                if (auto res = co_await m_conn.reconnect(); res) {
                    if (auto cache = m_fs.cache(); cache) {
                        co_await cache->invalidate_fds(false);
                    }
                    co_await m_conn.start();
                }
            } else if (not m_conn.is_optimal()) {
                log_i(__func__, "connection is ok but not optimized. trying to optimize...");
                if (auto res = co_await m_conn.optimize(); res) {
                    if (auto cache = m_fs.cache(); cache) {
                        co_await cache->invalidate_fds(false);
                    }
                    co_await m_conn.start();
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

            if (auto cache = m_fs.cache(); cache) {
                co_await cache->clean_stale_fds();
            }

            const auto& handles = m_fs.handles();

            auto cap  = handles.capacity();
            auto open = handles.size();

            log_i(__func__, "file handles [cap={:>04d}|open={:>04d}|empty={:>04d}]", cap, open, cap - open);
        }
    }
}

namespace
{
    void async_loop(async::Context& ctx)
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

    int run_fuse(fuse_session* se, args::MountOpt::Fuse& opt)
    {
        if (opt.singlethread) {
            return fuse_session_loop(se);
        }

        auto config = fuse_loop_cfg_create();
        auto defer  = util::defer([&] { fuse_loop_cfg_destroy(config); });

        fuse_loop_cfg_set_clone_fd(config, opt.clone_fd);
        fuse_loop_cfg_set_max_threads(config, opt.n_threads);

        return fuse_session_loop_mt(se, config);
    }
}

namespace madbfs::ll
{
    int run(args::MountOpt& opt)
    {
        namespace strat = connection_strategy;

        auto bridge = Bridge{ opt.fuse.mount, opt.app.writeback_cache };

        auto oper = &operations::operations;
        auto se   = fuse_session_new(&opt.fuse.args.inner(), oper, sizeof(*oper), &bridge);
        if (se == nullptr) {
            fmt::println(stderr, "error: failed to create fuse session");
            return 1;
        }
        auto fuse_se_deferrer = util::defer([&] { fuse_session_destroy(se); });

        if (::fuse_set_signal_handlers(se) != 0) {
            fmt::println(stderr, "error: failed to set fuse signal handler");
            return 1;
        }
        auto fuse_sig_deferrer = util::defer([&] { fuse_remove_signal_handlers(se); });

        if (::fuse_session_mount(se, opt.fuse.mount.c_str()) != 0) {
            fmt::println(stderr, "error: failed to mount filesystem at {}", opt.fuse.mount);
            return 1;
        }
        auto fuse_mnt_deferrer = util::defer([&] { fuse_session_unmount(se); });

        if (::fuse_daemonize(opt.fuse.foreground) != 0) {
            fmt::println(stderr, "error: failed to daemonize filesystem");
            return 1;
        }

        auto ctx    = async::Context{ 1 };    // the whole async runtime is 1 thread
        auto guard  = async::WorkGuard{ ctx.get_executor() };
        auto thread = std::jthread{ async_loop, std::ref(ctx) };

        auto conn = opt.app.connection.visit(Overload{
            [&](args::AdbOnly) { return Connection{ ctx, strat::Adb{} }; },
            [&](args::NoServer c) { return Connection{ ctx, strat::Proxy{ {}, c.port } }; },
            [&](args::Server c) { return Connection{ ctx, strat::Proxy{ c.abi, c.port } }; },
        });
        async::block(ctx, conn.start());

        auto cache = opt.app.caching.transform([&](const auto& c) {
            auto page_size = c.pagesize * 1024;
            auto max_pages = c.cachesize * 1024 / c.pagesize;
            return async::block(ctx, cache::LruCache::create(conn, page_size, max_pages));
        });
        async::block(ctx, cache->initialize());

        auto ptr = cache.transform([](auto& c) { return &c; }).value_or(nullptr);
        auto fs  = async::block(ctx, Filesystem::create(conn, ptr, opt.app.root));

        auto ttl     = opt.app.ttl < 1 ? std::nullopt : Opt<Seconds>{ opt.app.ttl };
        auto timeout = opt.app.timeout < 1 ? std::nullopt : Opt<Seconds>{ opt.app.timeout };

        auto madbfs = Madbfs{ se, ctx, conn, *fs, opt.app.root, opt.fuse.mount, ttl, timeout };
        if (auto res = async::block(ctx, madbfs.start()); not res) {
            fmt::println(stderr, "error: failed to start filesystem: {}", err_msg(res.error()));
            return 1;
        }

        bridge.set_ctx(&ctx);
        bridge.set_madbfs(&madbfs);

        auto ret = run_fuse(se, opt.fuse);

        madbfs.stop();
        if (cache) {
            async::block(ctx, cache->shutdown());
        }

        // connection is stopped last, since madbfs's internal loop and cache requires it to be running
        conn.cancel(Errc::operation_canceled);

        guard.reset();
        ctx.stop();
        thread.join();

        return ret;
    }
}
