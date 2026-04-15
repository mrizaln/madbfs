#pragma once

#include "madbfs-server/request_handler.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    class Connection
    {
    public:
        Connection(rpc::Socket socket)
            : m_socket{ std::move(socket) }
            , m_channel{ m_socket.get_executor() }
            , m_pool{ 1 }
        {
        }

        ~Connection(){ stop(); }

        AExpect<void> run();

        void stop();

    private:
        struct Promise
        {
            Vec<u8>        buffer;
            rpc::Procedure procedure;
        };

        using Inflight = std::unordered_map<rpc::Id, Promise, rpc::Id::Hash>;
        using Channel  = async::Channel<Tup<rpc::Id, Var<rpc::Status, rpc::Response>>>;

        Await<Var<rpc::Status, rpc::Response>> handle_request(Vec<u8>& buffer, rpc::Request req);

        AExpect<void> send_response();

        rpc::Socket      m_socket;
        Channel          m_channel;
        Inflight         m_requests;
        net::thread_pool m_pool;

        RequestHandler m_handler;
        bool           m_running = false;
    };

    class Server
    {
    public:
        using HandlerSig = Await<Var<rpc::Status, rpc::Response>>(Vec<u8>& buffer, rpc::Request request);
        using Handler    = std::function<HandlerSig>;

        Server(async::Context& context, u16 port) noexcept(false);
        ~Server();

        Server(Server&&)            = delete;
        Server& operator=(Server&&) = delete;

        Server(const Server&)            = delete;
        Server& operator=(const Server&) = delete;

        /**
         * @brief Start listening for RPC requests.
         *
         * @param handler The procedure handler.
         */
        AExpect<void> run();

        void stop();

    private:
        AExpect<void> handle_connection(async::tcp::Socket sock);

        async::tcp::Acceptor m_acceptor;
        Opt<Connection>      m_connection;
        bool                 m_running = false;
    };
}
