#include "madbfs/connection/server_connection.hpp"

#include "madbfs/cmd.hpp"
#include "madbfs/path.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::connection
{
    AExpect<Uniq<rpc::Client>> ServerConnection::make_client(u16 port)
    {
        auto exec   = co_await async::current_executor();
        auto socket = async::tcp::Socket{ exec };

        auto address  = asio::ip::address_v4{ { 127, 0, 0, 1 } };    // localhost
        auto endpoint = asio::ip::tcp::endpoint{ address, port };

        if (auto res = co_await socket.async_connect(endpoint); not res) {
            log_e("{}: failed to connect to server at port {}", __func__, port);
            auto errc = async::to_generic_err(res.error(), Errc::not_connected);
            co_return Unexpect{ errc };
        }

        if (auto res = co_await rpc::handshake(socket, true); not res) {
            co_return Unexpect{ res.error() };
        }

        co_return std::make_unique<rpc::Client>(std::move(socket));
    }

    AExpect<rpc::Response> ServerConnection::send(Vec<u8>& buf, rpc::Request req)
    {
        if (m_client == nullptr) {
            log_i("{}: client is not connected, trying to reestablish connection", __func__);
            auto client = co_await make_client(m_port);
            if (not client) {
                log_e("{}: reconnection failed", __func__);
                co_return Unexpect{ client.error() };
            }
            m_client = std::move(*client);
            log_i("{}: reconnection successful", __func__);
        }

        if (not m_client->running()) {
            co_await m_client->start();
        }

        auto fut = co_await m_client->send_req(buf, std::move(req));
        if (not fut) {
            if (fut.error() == Errc::not_connected or fut.error() == Errc::broken_pipe) {
                log_e("{}: client is disconnected, releasing client", __func__);
                m_client.reset();
            }
            co_return Unexpect{ fut.error() };
        }

        auto res = co_await fut->async_extract();
        if (not res) {
            if (res.error() == Errc::not_connected or res.error() == Errc::broken_pipe) {
                log_e("{}: client is disconnected, releasing client", __func__);
                m_client.reset();
            }
            co_return Unexpect{ res.error() };
        }

        co_return res;
    }

    AExpect<Uniq<ServerConnection>> ServerConnection::prepare_and_create(Opt<path::Path> server, u16 port)
    {
        namespace bp = boost::process::v2;

        auto exec      = co_await async::current_executor();
        auto serv_file = Str{ "/data/local/tmp/madbfs-server" };

        // enable port forwarding
        auto forward = fmt::format("tcp:{}", port);
        if (auto res = co_await cmd::exec({ "adb", "forward", forward, forward }); not res) {
            auto msg = std::make_error_code(res.error()).message();
            log_e("{}: failed to enable port forwarding at port {}: {}", __func__, port, msg);
            co_return Unexpect{ res.error() };
        }

        if (not server) {
            log_i("{}: server path not set, try connect", __func__);
            auto client = co_await make_client(port);
            if (not client) {
                co_return Unexpect{ client.error() };
            }

            log_i("{}: server is already running, continue normally", __func__);
            co_return Uniq<ServerConnection>{ new ServerConnection{ port, std::move(*client) } };
        }

        log_i("{}: server path set to {}, pushing server normally", __func__, server->fullpath());

        // push server executable to device
        if (auto res = co_await cmd::exec({ "adb", "push", server->fullpath(), serv_file }); not res) {
            auto msg = std::make_error_code(res.error()).message();
            log_e("{}: failed to push 'madbfs-server' to device: {}", __func__, msg);
            co_return Unexpect{ res.error() };
        }

        // update execute permission
        if (auto res = co_await cmd::exec({ "adb", "shell", "chmod", "+x", serv_file }); not res) {
            auto msg = std::make_error_code(res.error()).message();
            log_e("{}: failed to update 'madbfs-server' permission: {}", __func__, msg);
            co_return Unexpect{ res.error() };
        }

        log_i("{}: trying to run server", __func__);

        // run server
        auto cmd      = bp::environment::find_executable("adb");
        auto port_str = fmt::format("{}", port);
        auto args     = Array<boost::string_view, 4>{
            "shell",
            { serv_file.data(), serv_file.size() },
            "--port",
            port_str,
        };

        auto cmds = args | sv::transform([](auto s) { return Str{ s.data(), s.size() }; });
        log_d("{}: executing adb {}", __func__, cmds);

        auto out  = Pipe{ exec };
        auto err  = Pipe{ exec };
        auto proc = Process{ exec, cmd, args, bp::process_stdio{ {}, out, err } };

        auto timer = async::Timer{ exec };
        timer.expires_after(std::chrono::seconds{ 5 });

        using namespace async::operators;

        auto buf   = String(rpc::server_ready_string.size(), '\0');
        auto waitd = co_await (timer.async_wait() || async::read_exact<char>(out, buf));

        if (waitd.index() == 0) {
            log_e("{}: waited for 5 seconds, server is timed out", __func__);
            co_return Unexpect{ Errc::timed_out };
        } else {
            auto n = std::get<1>(waitd);
            if (not n) {
                log_e("{}: failed to read output: {}", __func__, n.error().message());
                co_return Unexpect{ async::to_generic_err(n.error(), Errc::not_connected) };
            } else if (n.value() != buf.size()) {
                log_e("{}: server process broken pipe", __func__);
                co_return Unexpect{ Errc::broken_pipe };
            } else if (buf != rpc::server_ready_string) {
                log_e("{}: server process is responding, but incorrect response: {:?}", __func__, buf);
                co_return Unexpect{ Errc::broken_pipe };
            }
        }

        auto client = co_await make_client(port);
        if (not client) {
            co_return Unexpect{ client.error() };
        }

        log_i("{}: server is running and ready to be used", __func__);

        co_return Uniq<ServerConnection>{ new ServerConnection{
            port,
            std::move(*client),
            std::move(proc),
            std::move(out),
            std::move(err),
        } };
    }

    ServerConnection::~ServerConnection()
    {
        if (m_client) {
            m_client->stop();
        }

        if (not m_server_proc) {
            return;
        }

        auto ec      = error_code{};
        auto running = m_server_proc->running(ec);
        if (ec) {
            log_w("{}: error terminating server: {}", __func__, ec.message());
            return;
        }

        if (running) {
            m_server_proc->terminate(ec);
            if (ec) {
                log_w("{}: error terminating server: {}", __func__, ec.message());
            } else {
                log_i("{}: successfully terminating server", __func__);
            }
        }
    }

    AExpect<Gen<ParsedStat>> ServerConnection::statdir(path::Path path)
    {
        auto buf  = Vec<u8>{};
        auto req  = rpc::req::Listdir{ .path = path.fullpath() };
        auto resp = co_await send_req(buf, req);
        if (not resp) {
            co_return Unexpect{ resp.error() };
        }

        auto generator = [](Vec<u8> buf, Vec<Pair<Str, rpc::resp::Stat>> entries) -> Gen<ParsedStat> {
            for (const auto& [name, stat] : entries) {
                co_yield ParsedStat{
                    .stat = data::Stat {
                        .links = stat.links,
                        .size  = stat.size,
                        .mtime = stat.mtime,
                        .atime = stat.atime,
                        .ctime = stat.ctime,
                        .mode  = stat.mode,
                        .uid   = stat.uid,
                        .gid   = stat.gid,
                    },
                    .path = name,       // names are stored in buf
                };
            }
        };

        co_return generator(std::move(buf), std::move(resp).value().entries);
    }

    AExpect<data::Stat> ServerConnection::stat(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Stat{ .path = path.fullpath() };

        co_return (co_await send_req(buf, req)).transform([](rpc::resp::Stat resp) {
            return data::Stat{
                .links = resp.links,
                .size  = resp.size,
                .mtime = resp.mtime,
                .atime = resp.atime,
                .ctime = resp.ctime,
                .mode  = resp.mode,
                .uid   = resp.uid,
                .gid   = resp.gid,
            };
        });
    }

    AExpect<path::PathBuf> ServerConnection::readlink(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Readlink{ .path = path.fullpath() };

        co_return (co_await send_req(buf, req)).transform([&](rpc::resp::Readlink resp) {
            return path::resolve(path.parent_path(), resp.target);
        });
    }

    AExpect<void> ServerConnection::mknod(path::Path path, mode_t mode, dev_t dev)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Mknod{ .path = path.fullpath(), .mode = mode, .dev = dev };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::mkdir(path::Path path, mode_t mode)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Mkdir{ .path = path.fullpath(), .mode = mode };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::unlink(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Unlink{ .path = path.fullpath() };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::rmdir(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Rmdir{ .path = path.fullpath() };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::rename(path::Path from, path::Path to, u32 flags)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Rename{ .from = from.fullpath(), .to = to.fullpath(), .flags = flags };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::truncate(path::Path path, off_t size)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Truncate{ .path = path.fullpath(), .size = size };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<usize> ServerConnection::read(path::Path path, Span<char> out, off_t offset)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Read{ .path = path.fullpath(), .offset = offset, .size = out.size() };

        co_return (co_await send_req(buf, req)).transform([&](rpc::resp::Read resp) {
            auto size = std::min(resp.read.size(), out.size());
            std::copy_n(resp.read.begin(), size, out.begin());
            return size;
        });
    }

    AExpect<usize> ServerConnection::write(path::Path path, Span<const char> in, off_t offset)
    {
        auto buf   = Vec<u8>{};
        auto bytes = Span{ reinterpret_cast<const u8*>(in.data()), in.size() };
        auto req   = rpc::req::Write{ .path = path.fullpath(), .offset = offset, .in = bytes };

        co_return (co_await send_req(buf, req)).transform(proj(&rpc::resp::Write::size));
    }

    AExpect<void> ServerConnection::utimens(path::Path path, timespec atime, timespec mtime)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Utimens{ .path = path.fullpath(), .atime = atime, .mtime = mtime };

        co_return (co_await send_req(buf, req)).transform(sink_void);
    }

    AExpect<usize> ServerConnection::copy_file_range(
        path::Path in,
        off_t      in_off,
        path::Path out,
        off_t      out_off,
        usize      size
    )
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::CopyFileRange{
            .in_path    = in.fullpath(),
            .in_offset  = in_off,
            .out_path   = out.fullpath(),
            .out_offset = out_off,
            .size       = size,
        };

        co_return (co_await send_req(buf, req)).transform(proj(&rpc::resp::CopyFileRange::size));
    }
}
