#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    class RequestHandler
    {
    public:
        RequestHandler(async::tcp::Socket& sock)
            : m_server{ sock, m_buffer }
        {
        }

        AExpect<void> dispatch();

    private:
        using Response = Var<rpc::Status, rpc::Response>;

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

        Vec<u8>     m_buffer;
        rpc::Server m_server;
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
