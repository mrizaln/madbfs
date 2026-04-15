#include "madbfs-server/server.hpp"

#include <madbfs-common/log.hpp>

namespace madbfs::server
{
    AExpect<void> Connection::run()
    {
        m_running = true;

        auto exec = co_await async::current_executor();
        async::spawn(exec, send_response(), [&](std::exception_ptr e, Expect<void> res) {
            log::log_exception(e, "send");
            if (not res) {
                log_e("send: finished with error: {}", err_msg(res.error()));
            }

            m_channel.cancel();
            m_channel.reset();
        });

        log_d("{}: listening for requests", __func__);

        while (m_running and m_channel.is_open()) {
            auto header = co_await rpc::receive_request_header(m_socket);
            if (not header) {
                log_e("{}: failed to read request header: {}", __func__, err_msg(header.error()));
                break;
            }

            auto buffer = Vec<u8>{};
            auto req    = co_await rpc::receive_request(m_socket, buffer, *header);
            if (not req) {
                log_e("{}: failed to receive request: {}", __func__, err_msg(req.error()));
                break;
            }

            auto [it, _] = m_requests.emplace(header->id, Promise{ std::move(buffer), req->proc() });
            log_d("{}: new request [{}] [{}]", __func__, header->id.inner(), to_string(*req));

            async::spawn(
                m_pool,
                handle_request(it->second.buffer, std::move(*req)),
                [&, id = header->id](std::exception_ptr e, Var<rpc::Status, rpc::Response> resp) {
                    log::log_exception(e, "handler");
                    async::spawn(
                        m_channel.get_executor(),
                        m_channel.async_send({}, { id, std::move(resp) }),
                        [](std::exception_ptr e, Expect<void, net::error_code> res) {
                            log::log_exception(e, "handler");
                            if (not res) {
                                log_e("handler: finished with error: {}", res.error().message());
                            }
                        }
                    );
                }
            );
        }

        m_pool.wait();
        log_d("{}: listening complete", __func__);

        co_return Expect<void>{};
    }

    void Connection::stop()
    {
        if (m_running) {
            m_running = false;
            m_pool.stop();
            m_pool.wait();
            m_socket.cancel();
            m_socket.close();
            m_channel.cancel();
            m_channel.close();
        }
    }

    Await<Var<rpc::Status, rpc::Response>> Connection::handle_request(Vec<u8>& buffer, rpc::Request req)
    {
        co_return std::move(req).visit([&](rpc::IsRequest auto&& req) {
            return m_handler.handle_req(buffer, std::move(req));
        });
    }

    AExpect<void> Connection::send_response()
    {
        auto payload_buf = Vec<u8>{};

        while (m_running and m_channel.is_open()) {
            auto id_resp = co_await m_channel.async_receive();
            if (not id_resp) {
                log_e("{}: failed to receive response from channel: {}", __func__, id_resp.error().message());
                co_return Unexpect{ async::to_generic_err(id_resp.error(), Errc::broken_pipe) };
            }

            auto [id, response] = std::move(*id_resp);
            log_d("{}: new response: {}", __func__, id.inner());

            if (auto req = m_requests.extract(id); not req.empty()) {
                auto& [_, proc] = req.mapped();
                log_d("{}: response is [{}]", __func__, to_string(proc));
                std::ignore     = co_await rpc::send_response(m_socket, payload_buf, proc, response, id);
            } else {
                log_e("{}: response incoming for id {} but no promise registered", __func__, id.inner());
            }
        }

        co_return Expect<void>{};
    }
}

namespace madbfs::server
{
    Server::Server(async::Context& context, u16 port) noexcept(false)
        : m_acceptor{ context, async::tcp::Endpoint{ async::tcp::Proto::v4(), port } }
    {
        m_acceptor.set_option(async::tcp::Acceptor::reuse_address(true));
        m_acceptor.listen(1);
    }

    Server::~Server()
    {
        if (m_running) {
            stop();
        }
    }

    AExpect<void> Server::run()
    {
        m_running = true;

        while (m_running) {
            auto sock = co_await m_acceptor.async_accept();
            if (not sock) {
                log_e("{}: failed to accept connection: {}", __func__, sock.error().message());
                break;
            }

            log_d("{}: new connection", __func__);

            if (not m_connection) {
                if (auto res = co_await rpc::handshake(*sock); not res) {
                    co_return Unexpect{ res.error() };
                }

                log_d("{}: connection ok", __func__);

                m_connection.emplace(std::move(*sock));

                auto exec = co_await async::current_executor();
                async::spawn(exec, m_connection->run(), [this](std::exception_ptr e, Expect<void> res) {
                    log::log_exception(e, "run");
                    log_e("run: connection terminated: {}", err_msg(res.error()));
                    m_connection.reset();
                });
            } else {
                std::ignore = co_await async::write_lv<char>(*sock, "BUSY");
            }
        }

        co_return Expect<void>{};
    }

    void Server::stop()
    {
        if (m_running) {
            m_running = false;
            if (m_connection) {
                m_connection->stop();
            }
            m_acceptor.cancel();
            m_acceptor.close();
        }
    }
}
