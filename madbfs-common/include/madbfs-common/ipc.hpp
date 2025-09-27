#pragma once

#include <spdlog/sinks/base_sink.h>
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
        struct Help            { };
        struct Version         { };
        struct Info            { };
        struct InvalidateCache { };
        struct SetPageSize     { usize kib; };
        struct SetCacheSize    { usize mib; };
        struct SetTTL          { usize sec; };
        struct SetTimeout      { usize sec; };
        struct SetLogLevel     { String lvl; };
        struct Logcat          { bool color; };
        // clang-format on

        namespace name
        {
            constexpr auto help             = "help";
            constexpr auto version          = "version";
            constexpr auto info             = "info";
            constexpr auto invalidate_cache = "invalidate_cache";
            constexpr auto set_page_size    = "set_page_size";
            constexpr auto set_cache_size   = "set_cache_size";
            constexpr auto set_ttl          = "set_ttl";
            constexpr auto set_timeout      = "set_timeout";
            constexpr auto set_log_level    = "set_log_level";
            constexpr auto logcat           = "logcat";
        }

        constexpr auto names = std::to_array({
            name::help,
            name::version,
            name::info,
            name::invalidate_cache,
            name::set_page_size,
            name::set_cache_size,
            name::set_ttl,
            name::set_timeout,
            name::set_log_level,
            name::logcat,
        });
    }

    /**
     * @class FsOp
     *
     * @brief Operations that involves the FS.
     */
    struct FsOp    //
        : util::VarWrapper<
              op::Info,
              op::InvalidateCache,
              op::SetPageSize,
              op::SetCacheSize,
              op::SetTTL,
              op::SetTimeout,
              op::SetLogLevel>
    {
        using VarWrapper::VarWrapper;
    };

    /**
     * @class Op
     *
     * @brief All possible operations through the IPC.
     */
    struct Op : util::VarWrapper<FsOp, op::Help, op::Version, op::Logcat>
    {
        using VarWrapper::VarWrapper;
    };
}

namespace madbfs::ipc
{
    using Socket = async::unix_socket::Socket;

    /**
     * @class LogcatSink
     *
     * @brief Logger sink for logcat operation.
     */
    class LogcatSink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        struct Msg
        {
            String message;
            isize  color_start;
            isize  color_end;
            usize  level;    // numeric repr of log level
        };

        LogcatSink() = default;

        /**
         * @brief Construct a LogcatSink.
         *
         * @param max_queue The maximum number of messages being queued.
         */
        LogcatSink(usize max_queue)
            : m_max_queue{ max_queue }
        {
        }

        /**
         * @brief Swap the underlying queue and return the front one.
         *
         * You should `clear()` the queue after use else the buffer will remain.
         */
        std::deque<Msg>& swap();

    protected:
        void sink_it_(const spdlog::details::log_msg& msg) override;
        void flush_() override { /* do nothing */ };

    private:
        Array<std::deque<Msg>, 2> m_queue     = {};
        usize                     m_index     = 0;
        usize                     m_max_queue = 1024;
    };

    struct LogcatSubscriber
    {
        Socket socket;
        bool   color;
    };

    /**
     * @class Client
     *
     * @brief IPC Client, can send operations to Server.
     */
    class Client
    {
    public:
        /**
         * @brief Connect to socket at `socket_path`.
         *
         * @param context Async context.
         * @param socket_path The socket path.
         */
        static Expect<Client> create(async::Context& context, Str socket_path);

        /**
         * @brief Stop the IPC client.
         */
        void stop();

        /**
         * @brief Send filesystem operation to server.
         *
         * @param op Filesystem operation.
         */
        AExpect<boost::json::value> send(FsOp op);

        /**
         * @brief Get help from server.
         */
        AExpect<boost::json::value> help();

        /**
         * @brief Get version from server.
         */
        AExpect<boost::json::value> version();

        /**
         * @brief Start logcat operation.
         *
         * @param opt Logcat option.
         */
        AExpect<Gen<AExpect<String>>> logcat(op::Logcat opt);

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
     *
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
         * @param socket_path Location of the to-be-created socket.
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
         * @brief Stop the IPC listener.
         */
        void stop();

        Str path() const { return m_socket_path; }

    private:
        Server(Str path, Acceptor acceptor)
            : m_socket_path{ path }
            , m_socket{ std::move(acceptor) }
            , m_logcat_timer{ m_socket.get_executor() }
        {
        }

        Await<void> run();
        Await<void> handle_peer(Socket sock);
        Await<void> logcat_handler();

        String   m_socket_path;
        Acceptor m_socket;
        OnFsOp   m_on_op;
        bool     m_running = false;

        Shared<LogcatSink>    m_logcat_sink;
        Vec<LogcatSubscriber> m_logcat_subscribers;
        async::Timer          m_logcat_timer;
    };
}
