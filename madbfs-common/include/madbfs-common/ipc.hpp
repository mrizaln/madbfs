#pragma once

#if not defined(MADBFS_BUILD_IPC)
#    error "macro MADBFS_BUILD_IPC is not defined, you should not include this header!"
#endif

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"

#include <functional>

namespace boost::json
{
    class value;
}

namespace madbfs::ipc
{
    using Socket = async::unix_socket::Socket;

    namespace op
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
    }

    using Op = Var<op::Help, op::Info, op::InvalidateCache, op::SetPageSize, op::SetCacheSize>;

    class Client
    {
    public:
        static Expect<Client> create(async::Context& context, Str socket_path);

        AExpect<boost::json::value> send(Op op);

    private:
        Client(Str socket_path, Socket socket)
            : m_socket_path{ socket_path }
            , m_socket{ std::move(socket) }
        {
        }

        String m_socket_path;
        Socket m_socket;
    };

    class Server
    {
    public:
        using Acceptor = async::unix_socket::Acceptor;
        using OnOp     = std::move_only_function<Await<boost::json::value>(ipc::Op op)>;

        /**
         * @brief Create IPC server.
         *
         * @param context Async context.
         */
        static Expect<Server> create(async::Context& context, Str socket_path);

        ~Server();

        Server(Server&&)            = default;
        Server& operator=(Server&&) = default;

        Server(const Server&)            = delete;
        Server& operator=(const Server&) = delete;

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

        Str path() const { return m_socket_path; }

    private:
        Server(Str path, Acceptor acceptor)
            : m_socket_path{ path }
            , m_socket{ std::move(acceptor) }
        {
        }

        Await<void> run();
        Await<void> handle_peer(Socket sock);

        String   m_socket_path;
        Acceptor m_socket;
        OnOp     m_on_op;
        bool     m_running = false;
    };
}
