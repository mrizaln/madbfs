#include "madbfs-common/ipc.hpp"
#include "madbfs-common/log.hpp"
#include "madbfs-common/util/overload.hpp"

#include <boost/json.hpp>

#include <filesystem>

namespace madbfs::ipc
{
    namespace json = boost::json;

    constexpr auto max_msg_len = 4 * 1024uz;    // 4 KiB

    AExpect<String> receive_message(Socket& sock)
    {
        auto buffer = String{};
        if (auto n = co_await async::read_lv(sock, buffer, max_msg_len); not n) {
            co_return Unexpect{ async::to_generic_err(n.error(), Errc::io_error) };
        }
        co_return buffer;
    }

    AExpect<void> send_message(Socket& sock, Str msg)
    {
        if (auto n = co_await async::write_lv<char>(sock, msg); not n) {
            co_return Unexpect{ async::to_generic_err(n.error(), Errc::not_connected) };
        }
        co_return Expect<void>{};
    }

    Expect<Op, String> parse_op(Str msg)
    {
        try {
            const auto json = json::parse(msg);
            const auto op   = json::value_to<String>(json.at("op"));

            if (op == op::names::help) {
                return Op{ op::Help{} };
            } else if (op == op::names::info) {
                return Op{ op::Info{} };
            } else if (op == op::names::invalidate_cache) {
                return Op{ op::InvalidateCache{} };
            } else if (op == op::names::set_page_size) {
                return Op{ op::SetPageSize{ .kib = json::value_to<u32>(json.at("value")) } };
            } else if (op == op::names::set_cache_size) {
                return Op{ op::SetCacheSize{ .mib = json::value_to<u32>(json.at("value")) } };
            } else if (op == op::names::set_ttl) {
                return Op{ op::SetTTL{ .sec = json::value_to<i32>(json.at("value")) } };
            }

            return Unexpect{ fmt::format("'{}' is not a valid operation, try 'help'", op) };
        } catch (const boost::system::system_error& e) {
            return Unexpect{ e.code().message() };
        } catch (...) {
            return Unexpect{ "unknown error" };
        }
    }
}

namespace madbfs::ipc
{
    Expect<Client> Client::create(async::Context& context, Str socket_path)
    {
        auto path = std::filesystem::absolute(socket_path);
        auto ep   = async::unix_socket::Endpoint{ path.c_str() };

        auto ec   = net::error_code{};
        auto sock = Socket{ context };

        sock.open(ep.protocol(), ec);
        sock.connect(ep, ec);

        if (ec) {
            log_e("{}: failed to connect to remote {:?}: {}", __func__, path.c_str(), ec.message());
            return Unexpect{ async::to_generic_err(ec, Errc::address_not_available) };
        }

        return Client{ path.c_str(), std::move(sock) };
    }

    AExpect<json::value> Client::send(Op op)
    {
        namespace n = op::names;

        auto overload = util::Overload{
            // clang-format off
            [&](op::Help           ) { return json::value{ { "op", n::help             }                      }; },
            [&](op::Info           ) { return json::value{ { "op", n::info             }                      }; },
            [&](op::InvalidateCache) { return json::value{ { "op", n::invalidate_cache }                      }; },
            [&](op::SetPageSize  op) { return json::value{ { "op", n::set_page_size    }, { "value", op.kib } }; },
            [&](op::SetCacheSize op) { return json::value{ { "op", n::set_cache_size   }, { "value", op.mib } }; },
            [&](op::SetTTL       op) { return json::value{ { "op", n::set_ttl          }, { "value", op.sec } }; },
            // clang-format on
        };

        auto op_json = std::visit(std::move(overload), op);
        auto res     = co_await send_message(m_socket, json::serialize(op_json));
        if (not res) {
            co_return Unexpect{ res.error() };
        }

        auto response_str = co_await receive_message(m_socket);
        if (not response_str) {
            co_return Unexpect{ response_str.error() };
        }

        try {
            co_return json::parse(*response_str);
        } catch (const std::exception& e) {
            log_e("{}: failed to deserialize response: {}", __func__, e.what());
            co_return Unexpect{ Errc::bad_message };
        }
    }
}

namespace madbfs::ipc
{
    Server::~Server()
    {
        if (m_running) {
            stop();
            auto sock = m_socket_path.c_str();
            if (::unlink(sock) < 0) {
                log_e("{}: failed to unlink socket: {} [{}]", __func__, sock, strerror(errno));
            }
        }
    }

    Expect<Server> Server::create(async::Context& context, Str socket_path)
    {
        auto path = std::filesystem::absolute(socket_path);
        auto ep   = async::unix_socket::Endpoint{ path.c_str() };

        auto ec  = net::error_code{};
        auto acc = Acceptor{ context };

        acc.open(ep.protocol(), ec);
        acc.set_option(Acceptor::reuse_address(true));
        acc.bind(ep, ec);
        acc.listen(acc.max_listen_connections, ec);

        if (ec) {
            log_e("{}: failed to construct acceptor {:?}: {}", __func__, path.c_str(), ec.message());
            return Unexpect{ async::to_generic_err(ec, Errc::address_not_available) };
        }

        return Server{ path.c_str(), std::move(acc) };
    }

    Await<void> Server::launch(OnOp on_op)
    {
        log_d("{}: ipc launched!", __func__);
        m_running = true;
        m_on_op   = std::move(on_op);
        co_await run();
    }

    void Server::stop()
    {
        m_running = false;
        m_socket.cancel();
        m_socket.close();
    }

    Await<void> Server::run()
    {
        while (m_running) {
            auto res = co_await m_socket.async_accept();
            if (not res) {
                log_e("{}: socket accept failed: {}", __func__, res.error().message());
                continue;
            }

            log_i("{}: new ipc connection from peer", __func__);
            co_await handle_peer(std::move(res).value());
        }
    }

    Await<void> Server::handle_peer(Socket sock)
    {
        auto op_str = co_await receive_message(sock);
        if (not op_str) {
            co_return;
        }

        log_d("{}: op sent by peer: {:?}", __func__, *op_str);

        auto op     = parse_op(op_str.value());
        auto status = Str{};
        auto value  = json::value{};

        if (op) {
            status = "success";
            value  = co_await m_on_op(*op);
        } else {
            status = "error";
            value  = op.error();
        }

        auto response = json::serialize(json::value{
            { "status", status },
            { "value", value },
        });

        if (auto res = co_await send_message(sock, response); not res) {
            const auto msg = std::make_error_code(res.error()).message();
            log_w("{}: failed to send message: {}", __func__, msg);
        }
    }
}
