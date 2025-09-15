#pragma once

#if not defined(MADBFS_BUILD_IPC)
#    error "macro MADBFS_BUILD_IPC is not defined, you should not include this header!"
#endif

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/util/var_wrapper.hpp"

#include <functional>

namespace boost::json
{
    class value;
}

namespace madbfs::ipc
{
    namespace op
    {
        // clang-format off

        // regular op
        struct Help            { };
        struct Info            { };
        struct InvalidateCache { };
        struct SetPageSize     { usize kib; };
        struct SetCacheSize    { usize mib; };
        struct SetTTL          { isize sec; };
        struct Logcat          { };

        // clang-format on

        namespace names
        {
            constexpr auto help             = "help";
            constexpr auto info             = "info";
            constexpr auto invalidate_cache = "invalidate_cache";
            constexpr auto set_page_size    = "set_page_size";
            constexpr auto set_cache_size   = "set_cache_size";
            constexpr auto set_ttl          = "set_ttl";
            constexpr auto logcat           = "logcat";
        }
    }

    /**
     * @class FsOp
     * @brief Operations that involves the FS.
     */
    struct FsOp
        : util::VarWrapper<op::Info, op::InvalidateCache, op::SetPageSize, op::SetCacheSize, op::SetTTL>
    {
        using VarWrapper::VarWrapper;
    };

    /**
     * @class Op
     * @brief All possible operations through the IPC.
     */
    struct Op : util::VarWrapper<FsOp, op::Help, op::Logcat>
    {
        using VarWrapper::VarWrapper;
    };
}

namespace madbfs::ipc
{
    using Socket = async::unix_socket::Socket;

    /**
     * @class Client
     * @brief IPC Client, can send operations to Server.
     */
    class Client
    {
    public:
        static Expect<Client> create(async::Context& context, Str socket_path);

        void stop();

        AExpect<boost::json::value>   send(FsOp op);
        AExpect<boost::json::value>   help();
        AExpect<Gen<AExpect<String>>> logcat();

    private:
        Client(Str socket_path, Socket socket)
            : m_socket_path{ socket_path }
            , m_socket{ std::move(socket) }
        {
        }

        String m_socket_path;
        Socket m_socket;
    };

    /**
     * @class Server
     * @brief IPC Server, provides information regarding the FS.
     */
    class Server
    {
    public:
        using Acceptor = async::unix_socket::Acceptor;
        using OnFsOp   = std::move_only_function<Await<boost::json::value>(ipc::FsOp op)>;

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
         * @brief Lauch the IPC and listen for request for any `FsOp`.
         *
         * @param on_op Operation request handler.
         *
         * The caller only need to handle `FsOp`, other op will be handled by the Server itself.
         */
        Await<void> launch(OnFsOp on_op);

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
        Await<void> logcat_handler(Socket sock);

        String   m_socket_path;
        Acceptor m_socket;
        OnFsOp   m_on_op;
        bool     m_running = false;
    };
}
