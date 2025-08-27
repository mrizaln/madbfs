#include "madbfs/data/ipc.hpp"

#include <madbfs-common/log.hpp>

#include <boost/json.hpp>

namespace madbfs::data
{
    constexpr auto max_msg_len = 4 * 1024uz;    // 4 KiB

    AExpect<String> receive(Ipc::Socket& sock)
    {
        auto buffer = String{};
        if (auto n = co_await async::read_lv(sock, buffer, max_msg_len); not n) {
            co_return Unexpect{ async::to_generic_err(n.error(), Errc::io_error) };
        }
        co_return buffer;
    }

    AExpect<void> send(Ipc::Socket& sock, Str msg)
    {
        if (auto n = co_await async::write_lv<char>(sock, msg); not n) {
            co_return Unexpect{ async::to_generic_err(n.error(), Errc::not_connected) };
        }
        co_return Expect<void>{};
    }

    Expect<ipc::Op, std::string> parse_msg(Str msg)
    {
        try {
            const auto json = boost::json::parse(msg);
            const auto op   = boost::json::value_to<std::string>(json.at("op"));

            if (op == ipc::names::help) {
                return ipc::Op{ ipc::Help{} };
            } else if (op == ipc::names::info) {
                return ipc::Op{ ipc::Info{} };
            } else if (op == ipc::names::invalidate_cache) {
                return ipc::Op{ ipc::InvalidateCache{} };
            } else if (op == ipc::names::set_page_size) {
                return ipc::Op{
                    ipc::SetPageSize{ .kib = boost::json::value_to<u32>(json.at("value")) },
                };
            } else if (op == ipc::names::set_cache_size) {
                return ipc::Op{
                    ipc::SetCacheSize{ .mib = boost::json::value_to<u32>(json.at("value")) },
                };
            }

            return Unexpect{ fmt::format("'{}' is not a valid operation, try 'help'", op) };
        } catch (const boost::system::system_error& e) {
            return Unexpect{ e.code().message() };
        } catch (...) {
            return Unexpect{ "unknown error" };
        }
    }
}

namespace madbfs::data
{
    Ipc::~Ipc()
    {
        if (m_running) {
            stop();
            auto sock = m_socket_path.as_path().fullpath();    // underlying is an std::string
            if (::unlink(sock.data()) < 0) {
                log_e("{}: failed to unlink socket: {} [{}]", __func__, sock, strerror(errno));
            }
        }
    }

    Expect<Ipc> Ipc::create(async::Context& context)
    {
        auto socket_path = [] -> String {
            const auto* res = std::getenv("XDG_RUNTIME_DIR");
            return res ? res : "/tmp";
        }();

        const auto* serial = std::getenv("ANDROID_SERIAL");
        if (serial == nullptr) {
            return Unexpect{ Errc::no_such_device };
        }

        socket_path += '/';
        socket_path += "madbfs@";
        socket_path += serial;
        socket_path += ".sock";

        auto path = path::create_buf(std::move(socket_path));
        if (not path.has_value()) {
            return Unexpect{ Errc::bad_address };
        }

        auto name = path->as_path().fullpath();
        auto ep   = async::unix_socket::Endpoint{ name };

        auto ec  = net::error_code{};
        auto acc = Acceptor{ context };

        acc.open(ep.protocol(), ec);
        acc.set_option(Acceptor::reuse_address(true));
        acc.bind(ep, ec);
        acc.listen(acc.max_listen_connections, ec);

        if (ec) {
            log_e("{}: failed to construct acceptor {:?}: {}", __func__, name, ec.message());
            return Unexpect{ async::to_generic_err(ec, Errc::address_not_available) };
        }

        return Ipc{ std::move(*path), std::move(acc) };
    }

    Await<void> Ipc::launch(OnOp on_op)
    {
        log_d("{}: ipc launched!", __func__);
        m_running = true;
        m_on_op   = std::move(on_op);
        co_await run();
    }

    void Ipc::stop()
    {
        m_running = false;
        m_socket.cancel();
        m_socket.close();
    }

    Await<void> Ipc::run()
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

    Await<void> Ipc::handle_peer(Socket sock)
    {
        auto op_str = co_await receive(sock);
        if (not op_str) {
            co_return;
        }

        log_d("{}: op sent by peer: {:?}", __func__, *op_str);

        auto op     = parse_msg(op_str.value());
        auto status = Str{};
        auto value  = boost::json::value{};

        if (op) {
            status = "success";
            value  = co_await m_on_op(*op);
        } else {
            status = "error";
            value  = op.error();
        }

        auto response = boost::json::serialize(boost::json::value{
            { "status", status },
            { "value", value },
        });

        if (auto res = co_await send(sock, response); not res) {
            const auto msg = std::make_error_code(res.error()).message();
            log_w("{}: failed to send message: {}", __func__, msg);
        }
    }
}
