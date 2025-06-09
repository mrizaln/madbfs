#include "madbfs/data/ipc.hpp"

#include <madbfs-common/log.hpp>

namespace madbfs::data::ipc::names
{
    constexpr auto help             = "help";
    constexpr auto invalidate_cache = "invalidate_cache";
    constexpr auto set_page_size    = "set_page_size";
    constexpr auto get_page_size    = "get_page_size";
    constexpr auto set_cache_size   = "set_cache_size";
    constexpr auto get_cache_size   = "get_cache_size";
}

namespace madbfs::data
{
    using LenInfo              = std::array<char, 4>;
    constexpr auto max_msg_len = 4 * 1024uz;    // 4 KiB

    AExpect<String> receive(Ipc::Socket& sock)
    {
        auto len_buffer  = LenInfo{};
        auto [ec, count] = co_await async::read_exact<char>(sock, len_buffer);
        if (ec) {
            log_w({ "{}: failed to read from peer: {}" }, __func__, ec.message());
            co_return Unexpect{ async::to_generic_err(ec) };
        } else if (count != len_buffer.size()) {
            co_return Unexpect{ Errc::connection_reset };
        }

        auto len = ::ntohl(std::bit_cast<u32>(len_buffer));
        if (len > max_msg_len) {
            Unexpect{ Errc::message_size };
        }

        auto buffer        = String(len, '\0');
        auto [ec1, count1] = co_await async::read_exact<char>(sock, buffer);
        if (ec1) {
            log_w({ "{}: failed to read from peer: {}" }, __func__, ec1.message());
            co_return Unexpect{ async::to_generic_err(ec1) };
        } else if (count1 != len) {
            co_return Unexpect{ Errc::connection_reset };
        }

        co_return Str{ buffer.data(), len };
    }

    AExpect<void> send(Ipc::Socket& sock, Str msg)
    {
        auto len     = std::bit_cast<LenInfo>(::htonl(static_cast<u32>(msg.size())));
        auto len_str = Str{ len.data(), len.size() };
        if (auto [ec, _] = co_await async::write_exact<char>(sock, len_str); ec) {
            log_w({ "{}: failed to write to peer: {}" }, __func__, ec.message());
            co_return Unexpect{ async::to_generic_err(ec) };
        }

        auto [ec, count] = co_await async::write_exact<char>(sock, msg);
        if (ec) {
            log_w({ "{}: failed to write to peer: {}" }, __func__, ec.message());
            co_return Unexpect{ async::to_generic_err(ec) };
        } else if (count != msg.size()) {
            co_return Unexpect{ Errc::connection_reset };
        }

        co_return Expect<void>{};
    }

    std::expected<ipc::Op, std::string> parse_msg(Str msg) noexcept(false)
    {
        try {
            const auto json = boost::json::parse(msg);
            const auto op   = boost::json::value_to<std::string>(json.at("op"));

            if (op == ipc::names::help) {
                return ipc::Op{ ipc::Help{} };
            } else if (op == ipc::names::invalidate_cache) {
                return ipc::Op{ ipc::InvalidateCache{} };
            } else if (op == ipc::names::set_page_size) {
                return ipc::Op{
                    ipc::SetPageSize{ .kib = boost::json::value_to<u32>(json.at("value").at("kib")) },
                };
            } else if (op == ipc::names::get_page_size) {
                return ipc::Op{ ipc::GetPageSize{} };
            } else if (op == ipc::names::set_cache_size) {
                return ipc::Op{
                    ipc::SetCacheSize{ .mib = boost::json::value_to<u32>(json.at("value").at("mib")) },
                };
            } else if (op == ipc::names::get_cache_size) {
                return ipc::Op{ ipc::GetCacheSize{} };
            }

            return std::unexpected{ fmt::format("'{}' is not a valid operation, try 'help'", op) };
        } catch (const boost::system::system_error& e) {
            return std::unexpected{ e.code().message() };
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
                log_e({ "{}: failed to unlink socket: {} [{}]" }, __func__, sock, strerror(errno));
            }
        }
    }

    Expect<Uniq<Ipc>> Ipc::create(async::Context& context) noexcept(false)
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

        try {
            auto acc = Acceptor{ context, ep };    // may throw
            return Uniq<Ipc>{ new Ipc{ std::move(*path), std::move(acc) } };
        } catch (const boost::system::system_error& e) {
            log_e({ "{}: failed to construct acceptor {:?}: {}" }, __func__, name, e.code().message());
            return Unexpect{ async::to_generic_err(e.code(), Errc::address_not_available) };
        }
    }

    Await<void> Ipc::launch(OnOp on_op)
    {
        log_d({ "{}: ipc launched!" }, __func__);
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
                log_e({ "{}: socket accept failed: {}" }, __func__, res.error().message());
                continue;
            }

            log_i({ "{}: new ipc connection from peer" }, __func__);
            co_await handle_peer(std::move(res).value());
        }
    }

    Await<void> Ipc::handle_peer(Socket sock)
    {
        auto op_str = co_await receive(sock);
        if (not op_str) {
            co_return;
        }

        log_d({ "{}: op sent by peer: {:?}" }, __func__, *op_str);
        auto op = parse_msg(op_str.value());

        if (op.has_value()) {
            auto response      = boost::json::object{};
            response["status"] = "success";
            response["value"]  = co_await m_on_op(*op);

            auto msg = boost::json::serialize(response);
            if (auto res = co_await send(sock, msg); not res) {
                const auto msg = std::make_error_code(res.error()).message();
                log_w({ "{}: failed to send message: {}" }, __func__, msg);
            }
        } else {
            auto json       = boost::json::object{};
            json["status"]  = "error";
            json["message"] = std::move(op).error();

            auto msg = boost::json::serialize(json);
            if (auto res = co_await send(sock, msg); not res) {
                const auto msg = std::make_error_code(res.error()).message();
                log_w({ "{}: failed to send message: {}" }, __func__, msg);
            }
        }
    }
}
