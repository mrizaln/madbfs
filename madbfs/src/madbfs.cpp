#include "madbfs/madbfs.hpp"

#include "madbfs/connection/adb_connection.hpp"
#include "madbfs/connection/server_connection.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/overload.hpp>

#include <boost/json.hpp>

namespace madbfs
{
    Uniq<connection::Connection> Madbfs::prepare_connection(
        async::Context& ctx,
        Opt<path::Path> server,
        u16             port,
        Opt<Seconds>    timeout
    )
    {
        auto coro = [=] noexcept -> Await<Uniq<connection::Connection>> {
            auto result = co_await connection::ServerConnection::prepare_and_create(server, port, timeout);
            if (not result) {
                auto msg = std::make_error_code(result.error()).message();
                log_c("prepare_connection: failed to construct ServerConnection: {}", msg);
                log_c("prepare_connection: falling back to AdbConnection");
                co_return std::make_unique<connection::AdbConnection>();
            }
            log_d("prepare_connection: successfully created ServerConnection");
            co_return std::move(*result);
        };

        return async::block(ctx, coro());
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
            const auto msg = std::make_error_code(ipc.error()).message();
            log_e("Madbfs: failed to initialize ipc: {}", msg);
            return {};
        }

        log_i("Madbfs: succesfully created ipc: {}", ipc->path());
        return std::move(*ipc);
    }

    void Madbfs::work_thread_function(async::Context& ctx)
    {
        try {
            log_i("Madbfs: io_context running...");
            auto num_handlers = ctx.run();
            log_i("Madbfs: io_context stopped with {} handlers executed", num_handlers);
        } catch (const std::exception& e) {
            log_w("Madbfs: io_context stopped with an exception: {}", e.what());
        } catch (...) {
            log_w("Madbfs: io_context stopped with an exception (unknown type)");
        }
    }

    Madbfs::Madbfs(
        Opt<path::Path> server,
        u16             port,
        usize           page_size,
        usize           max_pages,
        Str             mountpoint,
        Opt<Seconds>    ttl,
        Opt<Seconds>    timeout
    )
        : m_async_ctx{}
        , m_work_guard{ m_async_ctx.get_executor() }
        , m_work_thread{ [this] { work_thread_function(m_async_ctx); } }
        , m_connection{ prepare_connection(m_async_ctx, server, port, timeout) }
        , m_cache{ *m_connection, page_size, max_pages }
        , m_tree{ *m_connection, m_cache, ttl }
        , m_ipc{ create_ipc(m_async_ctx) }
        , m_mountpoint{ mountpoint }
    {
        if (m_ipc) {
            auto coro = m_ipc->launch([this](ipc::FsOp op) { return ipc_handler(op); });
            async::spawn(m_async_ctx, std::move(coro), [](std::exception_ptr e) {
                log::log_exception(e, "Madbfs");
            });
        }
    }

    Madbfs::~Madbfs()
    {
        if (m_ipc) {
            m_ipc->stop();
        }

        async::block(m_async_ctx, m_cache.shutdown());

        m_work_guard.reset();
        m_async_ctx.stop();
        m_work_thread.join();
    }

    Await<boost::json::value> Madbfs::ipc_handler(ipc::FsOp op)
    {
        namespace json = boost::json;

        constexpr usize lowest_page_size  = 64 * 1024;
        constexpr usize highest_page_size = 4 * 1024 * 1024;
        constexpr usize lowest_max_pages  = 128;

        auto overload = util::Overload{
            [&](ipc::op::Info) -> Await<json::value> {
                const auto page_size     = m_cache.page_size();
                const auto max_pages     = m_cache.max_pages();
                const auto current_pages = m_cache.current_pages();
                const auto ttl_sec       = m_tree.ttl().transform(&Seconds::count);
                const auto timeout_sec   = m_connection->timeout().transform(&Seconds::count);

                co_return json::value{
                    { "connection", m_connection->name() },
                    { "log_level", log::level_to_str(log::get_level()) },
                    { "ttl", ttl_sec.value_or(0) },
                    { "timeout", timeout_sec.value_or(0) },
                    { "page_size", page_size / 1024 },
                    { "cache_size",
                      { { "max", page_size * max_pages / 1024 / 1024 },
                        { "current", page_size * current_pages / 1024 / 1024 } } },
                };
            },
            [&](ipc::op::InvalidateCache) -> Await<json::value> {
                const auto page_size     = m_cache.page_size();
                const auto current_pages = m_cache.current_pages();

                co_await m_cache.invalidate_all();

                co_return json::value{ { "size", page_size * current_pages / 1024 / 1024 } };
            },
            [&](ipc::op::SetPageSize size) -> Await<json::value> {
                const auto old_size = m_cache.page_size();
                const auto old_max  = m_cache.max_pages();

                auto new_size = std::bit_ceil(size.kib * 1024);
                new_size      = std::clamp(new_size, lowest_page_size, highest_page_size);

                auto new_max = std::bit_ceil(old_max * old_size / new_size);
                new_max      = std::max(new_max, lowest_max_pages);

                co_await m_cache.set_page_size(new_size);
                co_await m_cache.set_max_pages(new_max);

                co_return json::value{
                    { "page_size",
                      { { "old", old_size / 1024 },    //
                        { "new", new_size / 1024 } } },
                    { "cache_size",
                      { { "old", old_max * old_size / 1024 / 1024 },
                        { "new", new_max * new_size / 1024 / 1024 } } },
                };
            },
            [&](ipc::op::SetCacheSize size) -> Await<json::value> {
                const auto page    = m_cache.page_size();
                const auto old_max = m_cache.max_pages();

                auto new_max = std::bit_ceil(size.mib * 1024 * 1024 / page);
                new_max      = std::max(new_max, lowest_max_pages);

                co_await m_cache.set_max_pages(new_max);

                co_return json::value{
                    { "cache_size",
                      { { "old", old_max * page / 1024 / 1024 },    //
                        { "new", new_max * page / 1024 / 1024 } } },
                };
            },
            [&](ipc::op::SetTTL ttl) -> Await<json::value> {
                const auto new_ttl = ttl.sec < 1 ? std::nullopt : Opt<Seconds>{ ttl.sec };
                const auto old_ttl = m_tree.set_ttl(new_ttl);

                co_return json::value{
                    { "ttl",
                      { { "old", old_ttl.transform(&Seconds::count).value_or(0) },    //
                        { "new", new_ttl.transform(&Seconds::count).value_or(0) } } },
                };
            },
            [&](ipc::op::SetTimeout ttl) -> Await<json::value> {
                const auto new_timeout = ttl.sec < 1 ? std::nullopt : Opt<Seconds>{ ttl.sec };
                const auto old_timeout = m_connection->set_timeout(new_timeout);

                co_return json::value{
                    { "timeout",
                      { { "old", old_timeout.transform(&Seconds::count).value_or(0) },    //
                        { "new", new_timeout.transform(&Seconds::count).value_or(0) } } },
                };
            },
            [&](ipc::op::SetLogLevel op) -> Await<json::value> {
                const auto prev_level = log::get_level();
                const auto new_level  = log::level_from_str(op.lvl).value_or(prev_level);

                log::set_level(new_level);

                co_return json::value{
                    { "log_level",
                      { { "old", log::level_to_str(prev_level) },    //
                        { "new", log::level_to_str(new_level) } } },
                };
            }
        };

        co_return co_await std::move(op).visit(overload);
    }
}
