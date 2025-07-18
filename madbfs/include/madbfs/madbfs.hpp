#pragma once

#include "madbfs/connection/connection.hpp"
#include "madbfs/data/ipc.hpp"
#include "madbfs/tree/file_tree.hpp"

#include <thread>

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
        Madbfs(Opt<path::Path> server, u16 port, usize max_pages, usize page_size);
        ~Madbfs();

        Madbfs(Madbfs&&)            = delete;
        Madbfs& operator=(Madbfs&&) = delete;

        Madbfs(const Madbfs&)            = delete;
        Madbfs& operator=(const Madbfs&) = delete;

        tree::FileTree&    tree() { return m_tree; }
        async::Context&    async_ctx() { return m_async_ctx; }
        const data::Cache& cache() const { return m_cache; }

    private:
        /**
         * @brief Prepare and create connection to device.
         *
         * @param ctx Async context.
         * @param server Server binary path.
         * @param port Port on which the server will be ran on.
         *
         * If the server binary path is set, this function will attempt to create a `ServerConnection` and
         * then fall back to `AdbConnection` if the connection failed. If it is not set, it will immediately
         * cerate `AdbConnection` instead. The returned value will never be null.
         */
        static Uniq<connection::Connection> prepare_connection(
            async::Context& ctx,
            Opt<path::Path> server,
            u16             port
        );

        /**
         * @brief Create an IPC (Unix socket).
         *
         * @param ctx Async context.
         */
        static Opt<data::Ipc> create_ipc(async::Context& ctx);

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
        Await<boost::json::value> ipc_handler(data::ipc::Op op);

        async::Context   m_async_ctx;
        async::WorkGuard m_work_guard;    // to prevent `async::Context` from returning immediately
        std::jthread     m_work_thread;

        Uniq<connection::Connection> m_connection;
        data::Cache                  m_cache;
        tree::FileTree               m_tree;
        Opt<data::Ipc>               m_ipc;
    };
}
