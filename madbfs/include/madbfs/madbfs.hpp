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
        Uniq<connection::Connection> prepare_connection(Opt<path::Path> server, u16 port);
        Uniq<data::Ipc>              create_ipc();
        void                         work_thread_function();

        Await<boost::json::value> ipc_handler(data::ipc::Op op);

        async::Context   m_async_ctx;
        async::WorkGuard m_work_guard;    // to prevent `io_context` from returning immediately
        std::jthread     m_work_thread;

        Uniq<connection::Connection> m_connection;
        data::Cache                  m_cache;
        tree::FileTree               m_tree;
        Uniq<data::Ipc>              m_ipc;
    };
}
