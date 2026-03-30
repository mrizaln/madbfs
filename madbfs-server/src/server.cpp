#include "madbfs-server/server.hpp"

#include <madbfs-common/log.hpp>

namespace madbfs::server
{
    {
            }

        }

    }

    {
        }
    }

    {
            }



                }

            }
        }

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
        log_i("{}: madbfs-server version {}", __func__, MADBFS_VERSION_STRING);
        log_i("{}: launching tcp server on port: {}", __func__, m_acceptor.local_endpoint().port());

        m_running = true;

        auto handler = RequestHandler{};

        while (m_running) {
            auto sock = co_await m_acceptor.async_accept();
            if (not sock) {
                log_e("{}: failed to accept connection: {}", __func__, sock.error().message());
                break;
            }

            if (auto res = co_await rpc::handshake(*sock); not res) {
                co_return Unexpect{ res.error() };
            }

            auto rpc  = rpc::Server{ std::move(*sock) };
            auto task = rpc.listen([&](Vec<u8>& buf, rpc::Request req) -> Await<RequestHandler::Response> {
                co_return std::move(req).visit([&](rpc::IsRequest auto&& req) {
                    return handler.handle_req(buf, std::move(req));
                });
            });

            if (auto res = co_await std::move(task); not res) {
                log_e("{}: rpc::Server::listen return with an error: {}", __func__, err_msg(res.error()));
            }
        }

        co_return Expect<void>{};
    }

    void Server::stop()
    {
        m_running = false;
        m_acceptor.cancel();
        m_acceptor.close();
    }
}
