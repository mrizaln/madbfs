#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    class Server
    {
    public:
        Server(async::Context& context, u16 port) noexcept(false);
        ~Server();

        AExpect<void> run();
        void          stop();

    private:
        AExpect<void> handle(async::tcp::Socket sock);

        // clang-format off
        AExpect<void> handle_req_listdir        (rpc::Server& serv, const rpc::req::Listdir&       req);
        AExpect<void> handle_req_stat           (rpc::Server& serv, const rpc::req::Stat&          req);
        AExpect<void> handle_req_readlink       (rpc::Server& serv, const rpc::req::Readlink&      req);
        AExpect<void> handle_req_mknod          (rpc::Server& serv, const rpc::req::Mknod&         req);
        AExpect<void> handle_req_mkdir          (rpc::Server& serv, const rpc::req::Mkdir&         req);
        AExpect<void> handle_req_unlink         (rpc::Server& serv, const rpc::req::Unlink&        req);
        AExpect<void> handle_req_rmdir          (rpc::Server& serv, const rpc::req::Rmdir&         req);
        AExpect<void> handle_req_rename         (rpc::Server& serv, const rpc::req::Rename&        req);
        AExpect<void> handle_req_truncate       (rpc::Server& serv, const rpc::req::Truncate&      req);
        AExpect<void> handle_req_read           (rpc::Server& serv, const rpc::req::Read&          req);
        AExpect<void> handle_req_write          (rpc::Server& serv, const rpc::req::Write&         req);
        AExpect<void> handle_req_utimens        (rpc::Server& serv, const rpc::req::Utimens&       req);
        AExpect<void> handle_req_copy_file_range(rpc::Server& serv, const rpc::req::CopyFileRange& req);
        // clang-format on

        async::tcp::Acceptor m_acceptor;
        std::atomic<bool>    m_running;
    };
}
