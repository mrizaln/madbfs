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

        RequestHandler() = default;

        Response handle_req(Vec<u8>& buf, rpc::req::Listdir req);
        Response handle_req(Vec<u8>& buf, rpc::req::Stat req);
        Response handle_req(Vec<u8>& buf, rpc::req::Readlink req);
        Response handle_req(Vec<u8>& buf, rpc::req::Mknod req);
        Response handle_req(Vec<u8>& buf, rpc::req::Mkdir req);
        Response handle_req(Vec<u8>& buf, rpc::req::Unlink req);
        Response handle_req(Vec<u8>& buf, rpc::req::Rmdir req);
        Response handle_req(Vec<u8>& buf, rpc::req::Rename req);
        Response handle_req(Vec<u8>& buf, rpc::req::Truncate req);
        Response handle_req(Vec<u8>& buf, rpc::req::Read req);
        Response handle_req(Vec<u8>& buf, rpc::req::Write req);
        Response handle_req(Vec<u8>& buf, rpc::req::Utimens req);
        Response handle_req(Vec<u8>& buf, rpc::req::CopyFileRange req);

    private:
        bool m_renameat2_impl       = true;
        bool m_copy_file_range_impl = true;
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
