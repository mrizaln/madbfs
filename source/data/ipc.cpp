#include "adbfsm/data/ipc.hpp"
#include "adbfsm/log.hpp"

#include <nlohmann/json.hpp>

namespace adbfsm::data::ipc::names
{
    constexpr auto help             = "help";
    constexpr auto invalidate_cache = "invalidate_cache";
    constexpr auto set_page_size    = "set_page_size";
    constexpr auto get_page_size    = "get_page_size";
    constexpr auto set_cache_size   = "set_cache_size";
    constexpr auto get_cache_size   = "get_cache_size";
}

namespace adbfsm::data
{
    using LenInfo              = std::array<char, 4>;
    constexpr auto max_msg_len = 4 * 1024uz;    // 4 KiB

    Expect<Str> receive(sockpp::unix_socket& sock, String& buffer)
    {
        auto len_buffer = LenInfo{};
        auto len_res    = sock.read_n(len_buffer.data(), len_buffer.size());
        if (not len_res) {
            return Unexpect{ static_cast<Errc>(len_res.error().value()) };
        } else if (len_res.value() != len_buffer.size()) {
            return Unexpect{ Errc::invalid_argument };
        }

        auto len = ::ntohl(std::bit_cast<u32>(len_buffer));
        if (len > max_msg_len) {
            Unexpect{ Errc::message_size };
        }

        if (buffer.size() < len) {
            buffer.resize(std::bit_ceil(len), '\0');
        }

        auto msg = sock.read_n(buffer.data(), len);
        if (not msg) {
            return Unexpect{ static_cast<Errc>(msg.error().value()) };
        } else if (msg.value() != len) {
            return Unexpect{ Errc::connection_reset };
        }

        return Str{ buffer.data(), len };
    }

    Expect<void> send(sockpp::unix_socket& sock, Str msg)
    {
        auto len = std::bit_cast<LenInfo>(::htonl(static_cast<u32>(msg.size())));
        if (auto res = sock.write_n(len.data(), len.size()); not res) {
            return Unexpect{ static_cast<Errc>(res.error().value()) };
        }

        auto res = sock.write_n(msg.data(), msg.size());
        if (not res) {
            return Unexpect{ static_cast<Errc>(res.error().value()) };
        } else if (res.value() != msg.size()) {
            return Unexpect{ Errc::connection_reset };
        }

        return {};
    }

    Opt<ipc::Op> parse_msg(Str msg) noexcept(false)
    {
        auto json = nlohmann::json::parse(msg);

        const auto op = json.at("op").get<String>();

        if (op == ipc::names::help) {
            return ipc::Op{ ipc::Help{} };
        } else if (op == ipc::names::invalidate_cache) {
            return ipc::Op{ ipc::InvalidateCache{} };
        } else if (op == ipc::names::set_page_size) {
            return ipc::Op{ ipc::SetPageSize{ .kib = json.at("value").at("kib") } };
        } else if (op == ipc::names::get_page_size) {
            return ipc::Op{ ipc::GetPageSize{} };
        } else if (op == ipc::names::set_cache_size) {
            return ipc::Op{ ipc::SetCacheSize{ .mib = json.at("value").at("mib") } };
        } else if (op == ipc::names::get_cache_size) {
            return ipc::Op{ ipc::GetCacheSize{} };
        }

        return std::nullopt;
    }
}

namespace adbfsm::data
{
    Ipc::~Ipc()
    {
        if (m_socket) {
            stop();
            auto sock = m_socket_path.as_path().fullpath();    // underlying is an std::string
            if (::unlink(sock.data()) < 0) {
                log_e({ "{}: failed to unlink socket: {} [{}]" }, __func__, sock, strerror(errno));
            }
        }
    }

    Expect<Ipc> Ipc::create()
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
        socket_path += "adbfsm@";
        socket_path += serial;
        socket_path += ".sock";

        auto acc = std::make_unique<sockpp::unix_acceptor>();
        if (auto res = acc->open(sockpp::unix_address{ socket_path }); not res) {
            return Unexpect{ static_cast<Errc>(res.error().value()) };
        }

        auto path = path::create_buf(std::move(socket_path));
        if (not path.has_value()) {
            return Unexpect{ Errc::bad_address };
        }

        return Ipc{ std::move(*path), std::move(acc) };
    }

    void Ipc::launch(OnOp on_op)
    {
        m_running = true;
        m_threadpool->run();
        m_thread = std::jthread{ [this, on_op = std::move(on_op)](std::stop_token st) mutable {
            run(st, std::move(on_op));
        } };
    }

    void Ipc::stop()
    {
        if (m_running) {
            m_socket->shutdown();
        }
    }

    void Ipc::run(std::stop_token st, OnOp on_op)
    {
        m_running = true;
        m_threadpool->run();

        auto buffer = String(1024, '0');

        while (not st.stop_requested()) {
            auto res = m_socket->accept();
            if (not res) {
                log_e({ "{}: socket accept failed: {}" }, __func__, res.error_message());
                continue;
            }

            auto sock = res.release();
            log_i({ "{}: new ipc connection from peer" }, __func__);

            auto op_str = receive(sock, buffer);
            if (not op_str) {
                const auto msg = std::make_error_code(op_str.error()).message();
                log_w({ "{}: failed to receve message from peer: {}" }, __func__, msg);
                continue;
            }

            try {
                auto op = parse_msg(op_str.value());
                if (not op.has_value()) {
                    m_threadpool->enqueue_detached([sock = std::move(sock)] mutable {
                        auto json       = nlohmann::json{};
                        json["status"]  = "error";
                        json["message"] = "invalid operation";

                        auto msg = nlohmann::to_string(json);
                        if (auto res = send(sock, msg); not res) {
                            const auto msg = std::make_error_code(res.error()).message();
                            log_w({ "run | threadpool: failed to send message: {}" }, msg);
                        }
                    });
                } else {
                    m_threadpool->enqueue_detached([&on_op, sock = std::move(sock), op = *op] mutable {
                        auto response      = nlohmann::json{};
                        response["status"] = "success";
                        response["value"]  = on_op(op);

                        auto msg = nlohmann::to_string(response);
                        if (auto res = send(sock, msg); not res) {
                            const auto msg = std::make_error_code(res.error()).message();
                            log_w({ "run | threadpool: failed to send message: {}" }, msg);
                        }
                    });
                }
            } catch (const std::exception& e) {
                m_threadpool->enqueue_detached([what = String{ e.what() }, sock = std::move(sock)] mutable {
                    auto json       = nlohmann::json{};
                    json["status"]  = "error";
                    json["message"] = std::move(what);

                    auto msg = nlohmann::to_string(json);
                    if (auto res = send(sock, msg); not res) {
                        const auto msg = std::make_error_code(res.error()).message();
                        log_w({ "run | threadpool: failed to send message: {}" }, msg);
                    }
                });
                continue;
            }
        }

        m_threadpool->stop(true);
    }
}
