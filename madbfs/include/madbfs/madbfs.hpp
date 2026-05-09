#pragma once

#include "madbfs/args.hpp"
#include "madbfs/connection.hpp"
#include "madbfs/filesystem.hpp"

#include <madbfs-common/ipc.hpp>

#include <thread>

struct fuse;

namespace madbfs
{
    /**
     * @class Madbfs
     *
     * @brief Main class of the filesystem.
     *
     * Everything is controlled from here. An instantiation of this class will live as long as the fs mounted.
     * The instance will be available as fuse_context's private data.
     */
    class Madbfs
    {
    public:
        Madbfs(
            struct fuse*     fuse,
            args::Connection connection,
            usize            max_pages,
            usize            page_size,
            Str              mount_point,
            Opt<Seconds>     ttl,
            Opt<Seconds>     timeout
        );

        ~Madbfs();

        Madbfs(Madbfs&&)            = delete;
        Madbfs& operator=(Madbfs&&) = delete;

        Madbfs(const Madbfs&)            = delete;
        Madbfs& operator=(const Madbfs&) = delete;

        Filesystem&     fs() { return m_fs; }
        async::Context& ctx() { return m_async_ctx; }

        Str mountpoint() const { return m_mountpoint; }

    private:
        struct IpcHandler;

        /**
         * @brief Prepare and create connection to device.
         *
         * @param ctx Async context.
         * @param connection Conneciton type to be used (transport).
         */
        static Connection prepare_connection(async::Context& ctx, args::Connection connection);

        /**
         * @brief Create an IPC server.
         *
         * @param ctx Async context.
         *
         * @return IPC server if success else `std::nullopt`.
         */
        static Opt<ipc::Server> create_ipc(async::Context& ctx);

        /**
         * @brief Function for work thread on which async context will run on.
         */
        static void work_thread_function(async::Context& ctx);

        /**
         * @brief IPC operation handler.
         *
         * @param op Requested operation.
         *
         * This function handles all requested operations from peers that comes from `m_ipc` instance.
         */
        Await<boost::json::value> ipc_handler(ipc::FsOp op);

        /**
         * @brief Watch connection status.
         *
         * This coroutine will attempt to reconnect the connection or optimize it based on the status of the
         * connection.
         */
        Await<void> watchdog();

        /**
         * @brief Clean unused real file descriptors from cache.
         */
        Await<void> reaper();

        struct fuse* m_fuse;

        async::Context   m_async_ctx;
        async::WorkGuard m_work_guard;    // to prevent `async::Context` from returning immediately
        std::jthread     m_work_thread;

        Connection       m_connection;
        Filesystem       m_fs;
        Opt<ipc::Server> m_ipc;

        async::Timer    m_watchdog_timer;
        async::Timer    m_reaper_timer;
        net::signal_set m_signal;

        String       m_mountpoint;
        Opt<Seconds> m_timeout;
    };
}
