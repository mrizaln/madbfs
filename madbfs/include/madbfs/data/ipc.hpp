#pragma once

#include "madbfs/path.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>

#include <functional>

namespace boost::json
{
    class value;
}

namespace madbfs::data
{
    namespace ipc
    {
        // clang-format off
        struct Help            { };
        struct Info            { };
        struct InvalidateCache { };
        struct SetPageSize     { usize kib; };
        struct SetCacheSize    { usize mib; };
        // clang-format on

        namespace names
        {
            constexpr auto help             = "help";
            constexpr auto info             = "info";
            constexpr auto invalidate_cache = "invalidate_cache";
            constexpr auto set_page_size    = "set_page_size";
            constexpr auto set_cache_size   = "set_cache_size";
        }

        using Op = Var<Help, Info, InvalidateCache, SetPageSize, SetCacheSize>;
    }

    class Ipc
    {
    public:
        using Acceptor = async::unix_socket::Acceptor;
        using Socket   = async::unix_socket::Socket;
        using OnOp     = std::move_only_function<Await<boost::json::value>(ipc::Op op)>;

        /**
         * @brief Create IPC.
         *
         * @param context Async context.
         */
        static Expect<Ipc> create(async::Context& context);

        ~Ipc();

        Ipc(Ipc&&)            = default;
        Ipc& operator=(Ipc&&) = default;

        Ipc(const Ipc&)            = delete;
        Ipc& operator=(const Ipc&) = delete;

        /**
         * @brief Lauch the IPC and listen for request.
         *
         * @param on_op Operation request handler.
         */
        Await<void> launch(OnOp on_op);

        /**
         * @brief Stop the IPC.
         */
        void stop();

        path::Path path() const { return m_socket_path.as_path(); }

    private:
        Ipc(path::PathBuf path, Acceptor acceptor)
            : m_socket_path{ std::move(path) }
            , m_socket{ std::move(acceptor) }
        {
        }

        Await<void> run();
        Await<void> handle_peer(Socket sock);

        path::PathBuf m_socket_path;
        Acceptor      m_socket;
        OnOp          m_on_op;
        bool          m_running = false;
    };
}
