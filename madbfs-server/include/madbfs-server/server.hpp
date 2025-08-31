#pragma once

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    class RequestHandler
    {
    public:
        using Response = Var<rpc::Status, rpc::Response>;

        RequestHandler(Vec<u8>& buffer)
            : m_buffer{ buffer }
        {
        }

        Response handle_req(rpc::req::Listdir req);
        Response handle_req(rpc::req::Stat req);
        Response handle_req(rpc::req::Readlink req);
        Response handle_req(rpc::req::Mknod req);
        Response handle_req(rpc::req::Mkdir req);
        Response handle_req(rpc::req::Unlink req);
        Response handle_req(rpc::req::Rmdir req);
        Response handle_req(rpc::req::Rename req);
        Response handle_req(rpc::req::Truncate req);
        Response handle_req(rpc::req::Read req);
        Response handle_req(rpc::req::Write req);
        Response handle_req(rpc::req::Utimens req);
        Response handle_req(rpc::req::CopyFileRange req);

    private:
        Vec<u8>& m_buffer;
    };

    class Server
    {
    public:
        Server(async::Context& context, u16 port) noexcept(false);
        ~Server();

        Server(Server&&)            = delete;
        Server& operator=(Server&&) = delete;

        Server(const Server&)            = delete;
        Server& operator=(const Server&) = delete;

        AExpect<void> run();
        void          stop();

    private:
        AExpect<void> handle_connection(async::tcp::Socket sock);

        async::tcp::Acceptor m_acceptor;
        std::atomic<bool>    m_running;
    };
}
