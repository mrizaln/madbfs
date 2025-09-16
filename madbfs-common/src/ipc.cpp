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
            } else if (op == op::names::logcat) {
                return op::Logcat{};
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
    LogcatSink::MsgQueue& LogcatSink::swap()
    {
        auto new_index = (m_index + 1) % 2;
        return m_queue[std::exchange(m_index, new_index)];
    }

    void LogcatSink::sink_it_(const spdlog::details::log_msg& msg)
    {
        auto buf = spdlog::memory_buf_t{};
        this->formatter_->format(msg, buf);

        auto& queue = m_queue[m_index];
        if (queue.size() == m_max_queue) {
            queue.pop_front();
        }
        queue.emplace_back(fmt::to_string(buf));
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

    void Client::stop()
    {
        m_socket.cancel();
        m_socket.close();
    }

    AExpect<json::value> Client::send(FsOp op)
    {
        namespace n = op::names;

        // clang-format off
        auto op_json = op.visit(util::Overload{
            [&](op::Info           ) { return json::value{ { "op", n::info             }                      }; },
            [&](op::InvalidateCache) { return json::value{ { "op", n::invalidate_cache }                      }; },
            [&](op::SetPageSize  op) { return json::value{ { "op", n::set_page_size    }, { "value", op.kib } }; },
            [&](op::SetCacheSize op) { return json::value{ { "op", n::set_cache_size   }, { "value", op.mib } }; },
            [&](op::SetTTL       op) { return json::value{ { "op", n::set_ttl          }, { "value", op.sec } }; },
        });
        // clang-format on

        if (auto res = co_await send_message(m_socket, json::serialize(op_json)); not res) {
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

    AExpect<json::value> Client::help()
    {
        auto op_json = json::value{ { "op", op::names::help } };
        if (auto res = co_await send_message(m_socket, json::serialize(op_json)); not res) {
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

    AExpect<Gen<AExpect<String>>> Client::logcat()
    {
        auto op_json = json::value{ { "op", op::names::logcat } };
        if (auto res = co_await send_message(m_socket, json::serialize(op_json)); not res) {
            co_return Unexpect{ res.error() };
        }

        co_return [sock = &m_socket](this auto) -> Gen<AExpect<String>> {
            while (sock->is_open()) {
                co_yield receive_message(*sock);
            }
        }();
    }
}

namespace madbfs::ipc
{
    Server::~Server()
    {
        if (m_running) {
            stop();
        }
        if (not m_socket_path.empty()) {
            if (auto sock = m_socket_path.c_str(); ::unlink(sock) < 0) {
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

    Await<void> Server::launch(OnFsOp on_op)
    {
        log_d("{}: ipc launched!", __func__);
        m_running = true;
        m_on_op   = std::move(on_op);

        std::ignore = co_await async::wait_all(run(), logcat_handler());
    }

    void Server::stop()
    {
        m_running = false;
        m_socket.cancel();
        m_socket.close();
        m_logcat_timer.cancel();
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
        // clang-format off
        static const auto help_json = json::value{
            { "operations", {
                ipc::op::names::help,
                ipc::op::names::logcat,
                ipc::op::names::info,
                ipc::op::names::invalidate_cache,
                ipc::op::names::set_page_size,
                ipc::op::names::set_cache_size,
                ipc::op::names::set_ttl,
            } },
        };
        // clang-format on

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

            switch (op->index()) {
            case 0: /* fs_op  */ value = co_await m_on_op(*op->as<FsOp>()); break;
            case 1: /* help   */ value = help_json; break;
            case 2: /* logcat */ m_logcat_subscribers.push_back(std::move(sock)); co_return;
            default: log_c("{}: [BUG] not all op variants are handled!", __func__); co_return;
            }
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

    Await<void> Server::logcat_handler()
    {
        using namespace std::chrono_literals;

        log_i("{}: start", __func__);

        if (not m_logcat_sink) {
            m_logcat_sink = std::make_shared<LogcatSink>();
            m_logcat_sink->set_level(log::Level::off);
            m_logcat_sink->set_pattern(log::logger_pattern);

            if (auto logger = spdlog::get(log::logger_name); not logger) {
                log_e("{}: can't find logger with name '{}'", __func__, log::logger_name);
                co_return;
            } else {
                auto& sinks = logger->sinks();
                sinks.push_back(m_logcat_sink);
            }
        }

        auto inactive_subscribers = Vec<isize>{};
        auto prev_empty           = m_logcat_subscribers.empty();

        while (m_running) {
            m_logcat_timer.expires_after(100ms);
            co_await m_logcat_timer.async_wait();

            if (m_logcat_subscribers.empty()) {
                prev_empty = true;
                continue;
            } else if (prev_empty) {
                m_logcat_sink->set_level(log::Level::debug);
                prev_empty = false;
            }

            auto& messages = m_logcat_sink->swap();

            for (auto&& [i, sock] : m_logcat_subscribers | sv::enumerate) {
                for (auto& message : messages) {
                    auto res = co_await send_message(sock, message);
                    if (not res) {
                        auto err_msg = std::make_error_code(res.error()).message();
                        inactive_subscribers.emplace_back(i);
                        break;
                    }
                }
            }

            for (auto idx : inactive_subscribers | sv::reverse) {
                m_logcat_subscribers.erase(m_logcat_subscribers.begin() + idx);
            }

            if (m_logcat_subscribers.empty()) {
                m_logcat_sink->set_level(log::Level::off);
            }

            inactive_subscribers.clear();
            messages.clear();
        }

        for (auto& sock : m_logcat_subscribers) {
            sock.cancel();
            sock.close();
        }

        m_logcat_subscribers.clear();

        log_i("{}: end", __func__);
    }
}
