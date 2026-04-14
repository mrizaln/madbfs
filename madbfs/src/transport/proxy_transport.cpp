#include "madbfs/transport/proxy_transport.hpp"
#include "madbfs/cmd.hpp"
#include "madbfs/path.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/rpc.hpp>

#define BOOST_PROCESS_VERSION 2
#include <boost/process.hpp>

namespace bp = boost::process::v2;

namespace
{
    using namespace madbfs;

    AExpect<rpc::Socket> connect_to_server(u16 port)
    {
        auto exec   = co_await async::current_executor();
        auto socket = async::tcp::Socket{ exec };

        auto address  = net::ip::address_v4{ { 127, 0, 0, 1 } };    // localhost
        auto endpoint = net::ip::tcp::endpoint{ address, port };

        if (auto res = co_await socket.async_connect(endpoint); not res) {
            log_e("{}: failed to connect to server at port {}", __func__, port);
            auto errc = async::to_generic_err(res.error(), Errc::not_connected);
            co_return Unexpect{ errc };
        }

        if (auto res = co_await rpc::handshake(socket); not res) {
            co_return Unexpect{ res.error() };
        }

        co_return socket;
    }

    AExpect<Tup<Opt<bp::process>, rpc::Socket>> launch_and_connect(Opt<path::Path> server, u16 port)
    {
        auto exec      = co_await async::current_executor();
        auto serv_file = Str{ "/data/local/tmp/madbfs-server" };

        // enable port forwarding
        auto forward = std::format("tcp:{}", port);
        if (auto res = co_await cmd::exec({ "adb", "forward", forward, forward }); not res) {
            log_e("{}: failed to enable forwarding at port {}: {}", __func__, port, err_msg(res.error()));
            co_return Unexpect{ res.error() };
        }

        if (not server) {
            log_i("{}: server path not set, try connect", __func__);
            auto socket = co_await async::timeout(connect_to_server(port), Seconds{ 5 });
            if (not socket) {
                co_return Unexpect{ socket.error() };
            }

            log_i("{}: server is already running, continue normally", __func__);
            co_return Tup{ Opt<bp::process>{}, std::move(*socket) };
        }

        log_i("{}: server path set to {}, pushing server normally", __func__, *server);

        // push server executable to device
        if (auto res = co_await cmd::exec({ "adb", "push", *server, serv_file }); not res) {
            log_e("{}: failed to push 'madbfs-server' to device: {}", __func__, err_msg(res.error()));
            co_return Unexpect{ res.error() };
        }

        // update execute permission
        if (auto res = co_await cmd::exec({ "adb", "shell", "chmod", "+x", serv_file }); not res) {
            log_e("{}: failed to update 'madbfs-server' permission: {}", __func__, err_msg(res.error()));
            co_return Unexpect{ res.error() };
        }

        log_i("{}: trying to run server", __func__);

        // run server
        auto cmd      = bp::environment::find_executable("adb");
        auto port_str = std::format("{}", port);
        auto args     = std::to_array<boost::string_view>({
            "shell",
            { serv_file.data(), serv_file.size() },
            "--port",
            port_str,
        });

        auto cmds = args | sv::transform([](auto s) { return Str{ s.data(), s.size() }; });
        log_d("{}: executing adb {}", __func__, cmds);

        auto out  = async::pipe::Read{ exec };
        auto err  = async::pipe::Read{ exec };
        auto proc = bp::process{ exec, cmd, args, bp::process_stdio{ {}, out, err } };

        auto buf = String(rpc::server_ready_string.size(), '\0');
        auto res = co_await async::timeout(async::read_exact<char>(out, buf), Seconds{ 5 });

        if (not res) {
            log_e("{}: waited for 5 seconds, server is timed out", __func__);
            co_return Unexpect{ Errc::timed_out };
        } else if (auto n = res.value(); not n) {
            log_e("{}: failed to read output: {}", __func__, n.error().message());
            co_return Unexpect{ async::to_generic_err(n.error(), Errc::not_connected) };
        } else if (n.value() != buf.size()) {
            log_e("{}: server process broken pipe", __func__);
            co_return Unexpect{ Errc::broken_pipe };
        } else if (buf != rpc::server_ready_string) {
            log_e("{}: server process is responding, but incorrect response: {:?}", __func__, buf);
            co_return Unexpect{ Errc::broken_pipe };
        }

        auto socket = co_await connect_to_server(port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        log_i("{}: server is running and ready to be used", __func__);

        co_return Tup{ std::move(proc), std::move(*socket) };
    }
}

namespace madbfs::transport
{
    struct ProxyTransport::Process
    {
        bp::process proc;
    };

    ProxyTransport::~ProxyTransport()
    {
        stop(Errc::operation_canceled);
    }

    AExpect<Uniq<ProxyTransport>> ProxyTransport::create(Opt<path::Path> server, u16 port)
    {
        auto conn = co_await launch_and_connect(server, port);
        if (not conn) {
            co_return Unexpect{ conn.error() };
        }

        auto [proc, sock] = std::move(*conn);
        co_return Uniq<ProxyTransport>{ new ProxyTransport{
            Uniq<Process>{ proc ? new Process{ std::move(*proc) } : nullptr },
            std::move(sock),
        } };
    }

    ProxyTransport::ProxyTransport(Uniq<Process> process, madbfs::rpc::Socket socket)
        : m_process{ std::move(process) }
        , m_socket{ std::move(socket) }
        , m_channel{ m_socket.get_executor() }
    {
    }

    void ProxyTransport::stop(rpc::Status status)
    {
        if (m_running) {
            m_running = false;

            for (auto& [id, promise] : m_requests) {
                promise.result.set_value(Unexpect{ status });
            }
            m_requests.clear();

            m_channel.cancel();
            m_channel.close();
        }

        if (m_socket.is_open()) {
            m_socket.cancel();
            m_socket.close();
        }

        if (m_process) {
            auto ec      = net::error_code{};
            auto running = m_process->proc.running(ec);
            if (ec) {
                log_w("{}: error terminating server: {}", __func__, ec.message());
                return;
            }

            if (running) {
                m_process->proc.terminate(ec);
                if (ec) {
                    log_w("{}: error terminating server: {}", __func__, ec.message());
                } else {
                    log_i("{}: successfully terminating server", __func__);
                }
            }
        }
    }

    Await<void> ProxyTransport::start()
    {
        log_d("{}: called", __func__);

        m_running = true;
        auto exec = co_await async::current_executor();

        async::spawn(exec, response_receive(), [&](std::exception_ptr e, Expect<void> res) {
            m_running = false;

            log::log_exception(e, "response_receive");
            if (not res) {
                log_w("response_receive: finished with error: {}", err_msg(res.error()));
            }

            if (not m_requests.empty()) {
                log_e("response_receive: there are {} promises unhandled", m_requests.size());
                for (auto& [id, p] : m_requests) {
                    p.result.set_value(Unexpect{ Errc::operation_canceled });
                }
            }

            m_requests.clear();
            m_channel.cancel();
            m_channel.reset();
        });

        async::spawn(exec, request_send(), [&](std::exception_ptr e, Expect<void> res) {
            log::log_exception(e, "request_send");
            if (not res) {
                log_w("request_send: finished with error: {}", err_msg(res.error()));
            }
        });
    }

    AExpect<rpc::Response> ProxyTransport::send(Vec<u8>& buffer, rpc::Request req)
    {
        if (not m_running) {
            co_return Unexpect{ Errc::resource_unavailable_try_again };
        }

        auto id      = next_id();
        auto promise = saf::promise<Expect<rpc::Response>>{ co_await async::current_executor() };
        auto future  = promise.get_future();

        m_requests.emplace(id, Promise{ buffer, std::move(promise) });

        if (auto res = co_await m_channel.async_send({}, { id, req }); not res) {
            log_e("{}: failed to send payload to channel: {}", __func__, res.error().message());
            co_return Unexpect{ async::to_generic_err(res.error(), Errc::broken_pipe) };
        }

        log_d("{}: REQ QUEUED {} [{}]", __func__, id.inner(), rpc::to_string(req));

        co_return co_await future.async_extract();
    }

    AExpect<rpc::Response> ProxyTransport::send(Vec<u8>& buffer, rpc::Request req, Milliseconds timeout)
    {
        if (not m_running) {
            co_return Unexpect{ Errc::resource_unavailable_try_again };
        }

        auto id      = next_id();
        auto promise = saf::promise<Expect<rpc::Response>>{ co_await async::current_executor() };
        auto future  = promise.get_future();

        m_requests.emplace(id, Promise{ buffer, std::move(promise) });

        if (auto res = co_await m_channel.async_send({}, { id, req }); not res) {
            log_e("{}: failed to send payload to channel: {}", __func__, res.error().message());
            co_return Unexpect{ async::to_generic_err(res.error(), Errc::broken_pipe) };
        }

        log_d("{}: REQ QUEUED {} [{}]", __func__, id.inner(), rpc::to_string(req));

        co_await async::timeout(future.async_wait(async::use_awaitable), timeout, [&] {
            if (auto entry = m_requests.extract(id); not entry.empty()) {
                log_d("{}: REQ CANCELLED {} [{}]", __func__, id.inner(), rpc::to_string(req));
                entry.mapped().result.set_value(Unexpect{ Errc::timed_out });
            }
        });

        co_return future.is_ready() ? future.extract() : Unexpect{ Errc::timed_out };
    }

    AExpect<void> ProxyTransport::request_send()
    {
        auto payload_buf = Vec<u8>{};

        while (m_running and m_channel.is_open()) {
            auto id_req = co_await m_channel.async_receive();
            if (not id_req) {
                log_e("{}: failed to recv payload from channel: {}", __func__, id_req.error().message());
                co_return Unexpect{ async::to_generic_err(id_req.error(), Errc::broken_pipe) };
            }

            auto [id, req] = std::move(*id_req);

            if (auto res = co_await rpc::send_request(m_socket, payload_buf, req, id); not res) {
                log_e("{}: failed to send request [{}]: {}", __func__, id.inner(), err_msg(res.error()));
                if (auto entry = m_requests.find(id); entry != m_requests.end()) {
                    entry->second.result.set_value(Unexpect{ res.error() });
                    m_requests.erase(entry);
                }
            }
        }

        co_return Expect<void>{};
    }

    AExpect<void> ProxyTransport::response_receive()
    {
        while (m_running) {
            auto header = co_await rpc::receive_response_header(m_socket);
            if (not header) {
                log_e("{}: failed to read response header: {}", __func__, err_msg(header.error()));
                co_return Unexpect{ header.error() };
            }

            log_d("{}: RESP RECV {} [{}]", __func__, header->id.inner(), rpc::to_string(header->proc));

            auto req = m_requests.extract(header->id);
            if (req.empty()) {
                log_e("{}: response incoming for id {} but no promise", __func__, header->id.inner());
                std::ignore = async::discard(m_socket, header->size);
                continue;
            }

            auto& [buf, res] = req.mapped();
            res.set_value(co_await rpc::receive_response(m_socket, buf, *header));
        }

        co_return Expect<void>{};
    }
}
