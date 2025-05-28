#include "madbfs-server/server.hpp"

#include <madbfs-common/rpc.hpp>
#include <madbfs-common/util/overload.hpp>

#include <spdlog/spdlog.h>

namespace
{
    std::string err_msg(madbfs::Errc errc)
    {
        return std::make_error_code(errc).message();
    }
}

namespace madbfs::server
{
    Server::Server(async::Context& context, u16 port) noexcept(false)
        : m_acceptor{ context, async::tcp::Endpoint{ async::tcp::Proto::v4(), port } }
    {
        m_acceptor.set_option(async::tcp::Acceptor::reuse_address(true));
        m_acceptor.listen();
    }

    Server::~Server()
    {
        if (m_running) {
            stop();
        }
    }

    AExpect<void> Server::run()
    {
        spdlog::info("{}: launching tcp server on port: {}", __func__, m_acceptor.local_endpoint().port());
        m_running = true;

        while (m_running) {
            auto sock = co_await m_acceptor.async_accept();
            if (not sock) {
                spdlog::error("{}: failed to accept connection: {}", __func__, sock.error().message());
                break;
            }

            auto exec = co_await async::this_coro::executor;
            async::spawn(exec, handle(std::move(*sock)), async::detached);
        }

        co_return Expect<void>{};
    }

    void Server::stop()
    {
        m_running = false;
        m_acceptor.cancel();
        m_acceptor.close();
    }

    AExpect<void> Server::handle(async::tcp::Socket sock)
    {
        auto ec   = std::error_code{};
        auto peer = sock.remote_endpoint(ec);
        if (ec) {
            spdlog::error("{}: failed to get endpoint: {}", __func__, ec.message());
        }
        spdlog::info("{}: new connection from {}:{}", __func__, peer.address().to_string(), peer.port());

        auto buffer     = Vec<u8>{};
        auto rpc_server = rpc::Server{ sock, buffer };

        auto request = co_await rpc_server.recv_req();
        if (not request) {
            spdlog::error("{}: failed to get param for procedure {}", __func__, err_msg(request.error()));
            co_return Expect<void>{};
        }

        spdlog::debug("{}: accepted procedure {}", __func__, request->index() + 1);

        co_return Expect<void>{};

        auto overload = util::Overload{
            // clang-format off
            [&](const rpc::req::Listdir&       req) { return handle_req_listdir        (rpc_server, req); },
            [&](const rpc::req::Stat&          req) { return handle_req_stat           (rpc_server, req); },
            [&](const rpc::req::Readlink&      req) { return handle_req_readlink       (rpc_server, req); },
            [&](const rpc::req::Mknod&         req) { return handle_req_mknod          (rpc_server, req); },
            [&](const rpc::req::Mkdir&         req) { return handle_req_mkdir          (rpc_server, req); },
            [&](const rpc::req::Unlink&        req) { return handle_req_unlink         (rpc_server, req); },
            [&](const rpc::req::Rmdir&         req) { return handle_req_rmdir          (rpc_server, req); },
            [&](const rpc::req::Rename&        req) { return handle_req_rename         (rpc_server, req); },
            [&](const rpc::req::Truncate&      req) { return handle_req_truncate       (rpc_server, req); },
            [&](const rpc::req::Read&          req) { return handle_req_read           (rpc_server, req); },
            [&](const rpc::req::Write&         req) { return handle_req_write          (rpc_server, req); },
            [&](const rpc::req::Utimens&       req) { return handle_req_utimens        (rpc_server, req); },
            [&](const rpc::req::CopyFileRange& req) { return handle_req_copy_file_range(rpc_server, req); },
            // clang-format on
        };

        co_return co_await std::visit(std::move(overload), *request);
    }
}
