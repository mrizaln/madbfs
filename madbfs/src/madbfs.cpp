#include "madbfs/madbfs.hpp"

#include "madbfs/connection/adb_connection.hpp"
#include "madbfs/connection/server_connection.hpp"
#include "madbfs/data/ipc.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/overload.hpp>

namespace madbfs
{
    Uniq<connection::Connection> Madbfs::prepare_connection(Opt<path::Path> server, u16 port)
    {
        auto coro = [=] -> Await<Uniq<connection::Connection>> {
            auto result = co_await connection::ServerConnection::prepare_and_create(server, port);
            if (not result) {
                auto msg = std::make_error_code(result.error()).message();
                log_c("prepare_connection: failed to construct ServerConnection: {}", msg);
                log_i("prepare_connection: falling back to AdbConnection");
                co_return std::make_unique<connection::AdbConnection>();
            }
            log_d("prepare_connection: successfully created ServerConnection");
            co_return std::move(*result);
        };

        auto fut = async::spawn(m_async_ctx, coro(), async::use_future);
        return fut.get();
    }

    Uniq<data::Ipc> Madbfs::create_ipc()
    {
        auto ipc = data::Ipc::create(m_async_ctx);
        if (not ipc.has_value()) {
            const auto msg = std::make_error_code(ipc.error()).message();
            log_e("Madbfs: failed to initialize ipc: {}", msg);
            return nullptr;
        }

        log_i("Madbfs: succesfully created ipc: {}", (*ipc)->path().fullpath());
        return std::move(*ipc);
    }

    void Madbfs::work_thread_function()
    {
        try {
            log_i("Madbfs: io_context running...");
            auto num_handlers = m_async_ctx.run();
            log_i("Madbfs: io_context stopped with {} handlers executed", num_handlers);
        } catch (std::exception& e) {
            log_w("Madbfs: io_context stopped with an exception: {}", e.what());
        }
    }

    Madbfs::Madbfs(Opt<path::Path> server, u16 port, usize page_size, usize max_pages)
        : m_async_ctx{}
        , m_work_guard{ m_async_ctx.get_executor() }
        , m_work_thread{ [this] { work_thread_function(); } }
        , m_connection{ prepare_connection(server, port) }
        , m_cache{ *m_connection, page_size, max_pages }
        , m_tree{ *m_connection, m_cache }
        , m_ipc{ create_ipc() }
    {
        if (m_ipc != nullptr) {
            auto coro = m_ipc->launch([this](data::ipc::Op op) { return ipc_handler(op); });
            async::spawn(m_async_ctx, std::move(coro), async::detached);
        }
    }

    Madbfs::~Madbfs()
    {
        auto fut = async::spawn(m_async_ctx, m_tree.shutdown(), async::use_future);
        fut.wait();

        m_work_guard.reset();
        m_async_ctx.stop();
        m_work_thread.join();
    }

    Await<boost::json::value> Madbfs::ipc_handler(data::ipc::Op op)
    {
        namespace ipc = data::ipc;

        constexpr usize lowest_page_size  = 64 * 1024;
        constexpr usize highest_page_size = 4 * 1024 * 1024;
        constexpr usize lowest_max_pages  = 128;

        auto overload = util::Overload{
            [&](ipc::Help) -> Await<boost::json::value> {
                auto json          = boost::json::object{};
                json["operations"] = {
                    "help",          "invalidate_cache", "set_page_size",
                    "get_page_size", "set_cache_size",   "get_cache_size",
                };
                co_return boost::json::value{ json };
            },
            [&](ipc::InvalidateCache) -> Await<boost::json::value> {
                co_await m_cache.invalidate_all();
                co_return boost::json::value{};
            },
            [&](ipc::SetPageSize size) -> Await<boost::json::value> {
                auto old_size = m_cache.page_size();
                auto new_size = std::bit_ceil(size.kib * 1024);
                new_size      = std::clamp(new_size, lowest_page_size, highest_page_size);
                co_await m_cache.set_page_size(new_size);

                auto old_max = m_cache.max_pages();
                auto new_max = std::bit_ceil(old_max * old_size / new_size);
                new_max      = std::max(new_max, lowest_max_pages);
                co_await m_cache.set_max_pages(new_max);

                auto json              = boost::json::object{};
                json["old_page_size"]  = old_size / 1024;
                json["old_cache_size"] = old_max * old_size / 1024 / 1024;
                json["new_page_size"]  = new_size / 1024;
                json["new_cache_size"] = new_max * new_size / 1024 / 1024;
                co_return boost::json::value{ json };
            },
            [&](ipc::GetPageSize) -> Await<boost::json::value> {
                auto size = m_cache.page_size();
                co_return boost::json::value(size);
            },
            [&](ipc::SetCacheSize size) -> Await<boost::json::value> {
                auto page    = m_cache.page_size();
                auto old_max = m_cache.max_pages();
                auto new_max = std::bit_ceil(size.mib * 1024 * 1024 / page);
                new_max      = std::max(new_max, lowest_max_pages);
                co_await m_cache.set_max_pages(new_max);

                auto json              = boost::json::object{};
                json["old_cache_size"] = old_max * page / 1024 / 1024;
                json["new_cache_size"] = new_max * page / 1024 / 1024;
                co_return boost::json::value{ json };
            },
            [&](ipc::GetCacheSize) -> Await<boost::json::value> {
                auto page      = m_cache.page_size();
                auto num_pages = m_cache.max_pages();
                co_return boost::json::value(page * num_pages / 1024 / 1024);
            },
        };

        co_return co_await std::visit(overload, op);
    }
}
