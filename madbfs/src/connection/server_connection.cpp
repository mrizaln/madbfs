#include "madbfs/connection/server_connection.hpp"

#include "madbfs-common/rpc.hpp"
#include "madbfs/log.hpp"
#include "madbfs/path/path.hpp"

namespace
{
    madbfs::AExpect<madbfs::async::tcp::Socket> connect(madbfs::u16 port)
    {
        auto exec   = co_await madbfs::async::this_coro::executor;
        auto socket = madbfs::async::tcp::Socket{ exec };

        auto address  = madbfs::asio::ip::address_v4{ { 127, 0, 0, 1 } };    // localhost
        auto endpoint = madbfs::asio::ip::tcp::endpoint{ address, port };

        if (auto res = co_await socket.async_connect(endpoint); not res) {
            madbfs::log_e({ "{}: failed to connect to server at port {}" }, __func__, port);
            co_return madbfs::Unexpect{ madbfs::async::to_generic_err(res.error()) };
        }

        co_return std::move(socket);
    }
}

namespace madbfs::connection
{
    using namespace std::string_view_literals;

    AExpect<Uniq<ServerConnection>> ServerConnection::prepare_and_create(u16 port)
    {
        log_d({ "{}: aaaaaaaaaaaaaaaaaaaaaaaa" }, __func__);
        namespace bp = boost::process::v2;

        auto exec = co_await async::this_coro::executor;

        // // push server executable to device
        // auto server_path =
        // "/home/mrizaln/Projects/C++/madbfs/madbfs-server/build/MinSizeRel/madbfs-server"; auto push_args =
        // Array<Str, 3>{ "push", server_path, "/data/local/tmp/" };    // trailing / if (auto res = co_await
        // exec_async("adb", push_args, "", true, false); not res) {
        //     auto msg = std::make_error_code(res.error()).message();
        //     log_e({ "{}: failed to push 'madbfs-server' to device: {}" }, __func__, msg);
        //     co_return Unexpect{ res.error() };
        // }

        // // update execute permission
        // auto chmod_args = Array<Str, 4>{ "shell", "chmod", "+x", "/data/local/tmp/madbfs-server" };
        // if (auto res = co_await exec_async("adb", chmod_args, "", true, false); not res) {
        //     auto msg = std::make_error_code(res.error()).message();
        //     log_e({ "{}: failed to update 'madbfs-server' permission: {}" }, __func__, msg);
        //     co_return Unexpect{ res.error() };
        // }

        // // enable port forwarding
        // auto forward      = fmt::format("tcp:{}", port);
        // auto forward_args = Array<Str, 3>{ "forward", forward, forward };
        // if (auto res = co_await exec_async("adb", forward_args, "", true, false); not res) {
        //     auto msg = std::make_error_code(res.error()).message();
        //     log_e({ "{}: failed to enable port forwarding at port {}: {}" }, __func__, port, msg);
        //     co_return Unexpect{ res.error() };
        // }

        // // run server
        // auto cmd      = bp::environment::find_executable("adb");
        // auto port_str = fmt::format("{}", port);
        // auto args     = Array<boost::string_view, 4>{
        //     "shell",
        //     "/data/local/tmp/madbfs-server",
        //     "--port",
        //     port_str,
        // };

        // auto out  = Pipe{ exec };
        // auto err  = Pipe{ exec };
        // auto proc = Process{ exec, cmd, args, bp::process_stdio{ {}, out, err } };

        // auto timer = async::Timer{ exec };
        // timer.expires_after(std::chrono::seconds{ 5 });

        // co_await timer.async_wait();
        // timer.expires_after(std::chrono::seconds{ 10 });

        // using namespace async::operators;

        // auto buf   = String(rpc::server_ready_string.size(), '\0');
        // auto waitd = co_await (timer.async_wait() || async::read_exact<char>(out, buf));

        // if (waitd.index() == 0) {
        //     log_e({ "{}: waited for 10 seconds, server is timed out" }, __func__);
        //     co_return Unexpect{ Errc::timed_out };
        // } else {
        //     auto [ec, n] = std::get<1>(waitd);
        //     if (ec) {
        //         log_e({ "{}: failed to read output: {}" }, __func__, ec.message());
        //         co_return Unexpect{ async::to_generic_err(ec) };
        //     } else if (n != buf.size()) {
        //         log_e({ "{}: server process broken pipe" }, __func__);
        //         co_return Unexpect{ Errc::broken_pipe };
        //     } else if (buf != rpc::server_ready_string) {
        //         log_e({ "{}: server process is responding, but incorrect response: {}" }, __func__, buf);
        //         co_return Unexpect{ Errc::broken_pipe };
        //     }
        // }

        // stub
        auto out  = Pipe{ exec };
        auto err  = Pipe{ exec };
        auto cmd  = bp::environment::find_executable("echo");
        auto proc = Process{ exec, cmd, {}, bp::process_stdio{ {}, out, err } };

        co_await proc.async_wait();

        log_d({ "{}: successfully created ServerConnection" }, __func__);

        co_return Uniq<ServerConnection>{ new ServerConnection{
            port,
            std::move(proc),
            std::move(out),
            std::move(err),
        } };
    }

    ServerConnection::~ServerConnection()
    {
        auto ec      = error_code{};
        auto running = m_server_proc.running(ec);
        if (ec) {
            log_w({ "{}: error terminating server: {}" }, __func__, ec.message());
        } else if (running) {
            m_server_proc.terminate(ec);
            log_w({ "{}: error terminating server: {}" }, __func__, ec.message());
        }
    }

    AExpect<Gen<ParsedStat>> ServerConnection::statdir(path::Path path)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Listdir{ .path = path.fullpath() };

        if (auto resp = co_await client.send_req_listdir(req); not resp) {
            co_return Unexpect{ resp.error() };
        }

        // this invalidates everything produced from client
        buffer.clear();
        auto listdir = rpc::listdir_channel::Receiver{ *socket, buffer };

        struct Slice
        {
            isize offset;
            usize size;
        };

        // TODO: use asynchronous generator so collecting is not necessary
        auto stats       = Vec<Pair<data::Stat, Slice>>{};
        auto name_buffer = Vec<char>{};

        while (true) {
            auto dirent = co_await listdir.recv_next();
            if (not dirent) {
                co_return Unexpect{ dirent.error() };
            }
            if (not(*dirent).has_value()) {
                break;
            }

            auto slice = Slice{ static_cast<isize>(name_buffer.size()), (*dirent)->name.size() };
            name_buffer.insert(name_buffer.end(), (*dirent)->name.begin(), (*dirent)->name.end());

            auto stat = data::Stat{
                .links = (*dirent)->links,
                .size  = (*dirent)->size,
                .mtime = (*dirent)->mtime,
                .atime = (*dirent)->atime,
                .ctime = (*dirent)->ctime,
                .mode  = (*dirent)->mode,
                .uid   = (*dirent)->uid,
                .gid   = (*dirent)->gid,
            };

            stats.emplace_back(std::move(stat), slice);
        }

        auto generator = [](Vec<char> buf, Vec<Pair<data::Stat, Slice>> stats) -> Gen<ParsedStat> {
            for (auto&& [stat, slice] : stats) {
                co_yield ParsedStat{ stat, Str{ buf.data() + slice.offset, slice.size } };
            }
        };

        co_return generator(std::move(name_buffer), std::move(stats));
    }

    AExpect<data::Stat> ServerConnection::stat(path::Path path)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Stat{ .path = path.fullpath() };

        co_return (co_await client.send_req_stat(req)).transform([](rpc::resp::Stat resp) {
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
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Readlink{ .path = path.fullpath() };

        co_return (co_await client.send_req_readlink(req)).transform([](rpc::resp::Readlink resp) {
            return path::create(resp.target).value().into_buf();
        });
    }

    AExpect<void> ServerConnection::mknod(path::Path path)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Mknod{ .path = path.fullpath() };

        co_return (co_await client.send_req_mknod(req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::mkdir(path::Path path)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Mkdir{ .path = path.fullpath() };

        co_return (co_await client.send_req_mkdir(req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::unlink(path::Path path)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Unlink{ .path = path.fullpath() };

        co_return (co_await client.send_req_unlink(req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::rmdir(path::Path path)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Rmdir{ .path = path.fullpath() };

        co_return (co_await client.send_req_rmdir(req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::rename(path::Path from, path::Path to)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Rename{ .from = from.fullpath(), .to = to.fullpath() };

        co_return (co_await client.send_req_rename(req)).transform(sink_void);
    }

    AExpect<void> ServerConnection::truncate(path::Path path, off_t size)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Truncate{ .path = path.fullpath(), .size = size };

        co_return (co_await client.send_req_truncate(req)).transform(sink_void);
    }

    AExpect<usize> ServerConnection::read(path::Path path, Span<char> out, off_t offset)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Read{ .path = path.fullpath(), .offset = offset, .size = out.size() };

        co_return (co_await client.send_req_read(req)).transform([&](rpc::resp::Read resp) {
            auto size = std::min(resp.read.size(), out.size());
            std::copy_n(resp.read.begin(), size, out.begin());
            return size;
        });
    }

    AExpect<usize> ServerConnection::write(path::Path path, Span<const char> in, off_t offset)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };

        auto in_bytes = Span{ reinterpret_cast<const u8*>(in.data()), in.size() };
        auto req      = rpc::req::Write{ .path = path.fullpath(), .offset = offset, .in = in_bytes };

        co_return (co_await client.send_req_write(req)).transform(proj(&rpc::resp::Write::size));
    }

    AExpect<void> ServerConnection::utimens(path::Path path, timespec atime, timespec mtime)
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };
        auto req    = rpc::req::Utimens{ .path = path.fullpath(), .atime = atime, .mtime = mtime };

        co_return (co_await client.send_req_utimens(req)).transform(sink_void);
    }

    AExpect<usize> ServerConnection::copy_file_range(
        path::Path in,
        off_t      in_off,
        path::Path out,
        off_t      out_off,
        usize      size
    )
    {
        auto socket = co_await connect(m_port);
        if (not socket) {
            co_return Unexpect{ socket.error() };
        }

        auto buffer = Vec<u8>{};
        auto client = rpc::Client{ *socket, buffer };

        auto req = rpc::req::CopyFileRange{
            .in_path    = in.fullpath(),
            .in_offset  = in_off,
            .out_path   = out.fullpath(),
            .out_offset = out_off,
            .size       = size,
        };

        auto resp = co_await client.send_req_copy_file_range(req);
        co_return resp.transform(proj(&rpc::resp::CopyFileRange::size));
    }
}
