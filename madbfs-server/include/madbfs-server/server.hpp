#pragma once

#include "madbfs-server/request_handler.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    /**
     * @class Connection
     *
     * @brief Represent active connection with madbfs client.
     */
    class Connection
    {
    public:
        /**
         * @brief Create a connection with associated socket.
         *
         * @param socket Socket to madbfs client.
         */
        Connection(rpc::Socket socket)
            : m_socket{ std::move(socket) }
            , m_channel{ m_socket.get_executor() }
            , m_pool{ 1 }
        {
        }

        ~Connection() { stop(); }

        /**
         * @brief Start handling requests from madbfs client.
         */
        AExpect<void> run();

        /**
         * @brief Stop handling requests.
         */
        void stop();

    private:
        struct Promise
        {
            Vec<u8>        buf;
            rpc::Procedure proc;
        };

        using Inflight = std::unordered_map<rpc::Id, Promise, rpc::Id::Hash>;
        using Channel  = async::Channel<Tup<rpc::Id, rpc::FallibleResponse>>;

        Await<rpc::FallibleResponse> handle_request(rpc::Request req);

        AExpect<void> send_response();

        rpc::Socket      m_socket;
        Channel          m_channel;
        Inflight         m_requests;
        net::thread_pool m_pool;

        RequestHandler m_handler;
        bool           m_running = false;
    };

    /**
     * @class Server
     *
     * @brief A connection listener for madbfs client.
     *
     * This class can only handle one `Connection` with madbfs client.
     */
    class Server
    {
    public:
        using HandlerSig = Await<rpc::FallibleResponse>(Vec<u8>& buffer, rpc::Request request);
        using Handler    = std::function<HandlerSig>;

        Server(async::Context& context, u16 port) noexcept(false);
        ~Server();

        Server(Server&&)            = delete;
        Server& operator=(Server&&) = delete;

        Server(const Server&)            = delete;
        Server& operator=(const Server&) = delete;

        /**
         * @brief Start listening for connection.
         */
        AExpect<void> run();

        /**
         * @brief Stop listening for connection.
         */
        void stop();

    private:
        async::tcp::Acceptor m_acceptor;
        Opt<Connection>      m_connection;
        bool                 m_running = false;
    };
}
