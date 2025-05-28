#include "madbfs-server/server.hpp"

#include <madbfs-common/rpc.hpp>
#include <madbfs-common/util/overload.hpp>

#include <spdlog/spdlog.h>

#include <dirent.h>
#include <sys/stat.h>

namespace
{
    std::string err_msg(madbfs::Errc errc)
    {
        return std::make_error_code(errc).message();
    }

    madbfs::rpc::Status log_status_from_errno(madbfs::Str name, madbfs::Str path, madbfs::Str msg)
    {
        auto err = errno;
        spdlog::debug("{}: {} {:?}: {}", name, msg, path, strerror(err));
        auto status = static_cast<madbfs::rpc::Status>(err);

        switch (status) {
        case madbfs::rpc::Status::Success:
        case madbfs::rpc::Status::NoSuchFileOrDirectory:
        case madbfs::rpc::Status::PermissionDenied:
        case madbfs::rpc::Status::FileExists:
        case madbfs::rpc::Status::NotADirectory:
        case madbfs::rpc::Status::IsADirectory:
        case madbfs::rpc::Status::InvalidArgument:
        case madbfs::rpc::Status::DirectoryNotEmpty: return status;
        }

        // revert to InvalidArgument as default
        return madbfs::rpc::Status::InvalidArgument;
    }
}

namespace madbfs::server
{
    Server::Server(async::Context& context, u16 port) noexcept(false)
        : m_acceptor{ context, async::tcp::Endpoint{ async::tcp::Proto::v4(), port } }
    {
        m_acceptor.set_option(async::tcp::Acceptor::reuse_address(true));
        m_acceptor.listen();
    }

    Server::~Server()
    {
        if (m_running) {
            stop();
        }
    }

    AExpect<void> Server::run()
    {
        spdlog::info("{}: launching tcp server on port: {}", __func__, m_acceptor.local_endpoint().port());
        m_running = true;

        while (m_running) {
            auto sock = co_await m_acceptor.async_accept();
            if (not sock) {
                spdlog::error("{}: failed to accept connection: {}", __func__, sock.error().message());
                break;
            }

            auto exec = co_await async::this_coro::executor;
            async::spawn(exec, handle(std::move(*sock)), async::detached);
        }

        co_return Expect<void>{};
    }

    void Server::stop()
    {
        m_running = false;
        m_acceptor.cancel();
        m_acceptor.close();
    }

    AExpect<void> Server::handle(async::tcp::Socket sock)
    {
        auto ec   = std::error_code{};
        auto peer = sock.remote_endpoint(ec);
        if (ec) {
            spdlog::error("{}: failed to get endpoint: {}", __func__, ec.message());
        }
        spdlog::debug("{}: new connection from {}:{}", __func__, peer.address().to_string(), peer.port());

        auto buffer     = Vec<u8>{};
        auto rpc_server = rpc::Server{ sock, buffer };

        auto procedure = co_await rpc_server.peek_req();
        if (not procedure) {
            spdlog::error("{}: failed to get param for procedure {}", __func__, err_msg(procedure.error()));
            co_return Expect<void>{};
        }
        spdlog::debug("{}: accepted procedure [{}]", __func__, to_string(*procedure));

        // clang-format off
        switch (*procedure) {
        case rpc::Procedure::Listdir:       co_return co_await handle_req_listdir        (rpc_server);
        case rpc::Procedure::Stat:          co_return co_await handle_req_stat           (rpc_server);
        case rpc::Procedure::Readlink:      co_return co_await handle_req_readlink       (rpc_server);
        case rpc::Procedure::Mknod:         co_return co_await handle_req_mknod          (rpc_server);
        case rpc::Procedure::Mkdir:         co_return co_await handle_req_mkdir          (rpc_server);
        case rpc::Procedure::Unlink:        co_return co_await handle_req_unlink         (rpc_server);
        case rpc::Procedure::Rmdir:         co_return co_await handle_req_rmdir          (rpc_server);
        case rpc::Procedure::Rename:        co_return co_await handle_req_rename         (rpc_server);
        case rpc::Procedure::Truncate:      co_return co_await handle_req_truncate       (rpc_server);
        case rpc::Procedure::Read:          co_return co_await handle_req_read           (rpc_server);
        case rpc::Procedure::Write:         co_return co_await handle_req_write          (rpc_server);
        case rpc::Procedure::Utimens:       co_return co_await handle_req_utimens        (rpc_server);
        case rpc::Procedure::CopyFileRange: co_return co_await handle_req_copy_file_range(rpc_server);
        }
        // clang-format on

        spdlog::error("{}: invalid procedure with integral value {}", __func__, static_cast<u8>(*procedure));

        co_return Expect<void>{};
    }

    AExpect<void> Server::handle_req_listdir(rpc::Server& serv)
    {
        auto listdir = co_await serv.recv_req_listdir();
        if (not listdir) {
            co_return Unexpect{ listdir.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, listdir->path.data());

        auto dir = ::opendir(listdir->path.data());
        if (dir == nullptr) {
            auto err = log_status_from_errno(__func__, listdir->path, "failed to open dir");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        if (auto res = co_await serv.send_resp(rpc::resp::Listdir{}); not res) {
            co_return Unexpect{ res.error() };
        }

        auto channel = rpc::listdir_channel::Sender{ serv.sock() };
        auto dirfd   = ::dirfd(dir);

        while (auto entry = ::readdir(dir)) {
            struct stat filestat = {};
            if (auto res = ::fstatat(dirfd, entry->d_name, &filestat, AT_SYMLINK_NOFOLLOW); res < 0) {
                auto err = log_status_from_errno(__func__, entry->d_name, "failed to stat file");
                if (auto res = co_await channel.send_next(static_cast<rpc::Status>(err)); not res) {
                    co_return Unexpect{ res.error() };
                }
                continue;
            }
            auto res = co_await channel.send_next(rpc::listdir_channel::Dirent{
                .name  = entry->d_name,
                .links = filestat.st_nlink,
                .size  = filestat.st_size,
                .mtime = filestat.st_mtim,
                .atime = filestat.st_atim,
                .ctime = filestat.st_ctim,
                .mode  = filestat.st_mode,
                .uid   = filestat.st_uid,
                .gid   = filestat.st_gid,
            });
            if (not res) {
                co_return Unexpect{ res.error() };
            }
        }

        co_return co_await channel.send_next(Unit{});
    }

    AExpect<void> Server::handle_req_stat(rpc::Server& serv)
    {
        auto stat = co_await serv.recv_req_stat();
        if (not stat) {
            co_return Unexpect{ stat.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, stat->path.data());

        struct stat filestat = {};
        if (auto res = ::lstat(stat->path.data(), &filestat); res < 0) {
            auto err = log_status_from_errno(__func__, stat->path, "failed to stat file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Stat{
            .size  = filestat.st_size,
            .links = filestat.st_nlink,
            .mtime = filestat.st_mtim,
            .atime = filestat.st_atim,
            .ctime = filestat.st_ctim,
            .mode  = filestat.st_mode,
            .uid   = filestat.st_uid,
            .gid   = filestat.st_gid,
        });
    }

    AExpect<void> Server::handle_req_readlink(rpc::Server& serv)
    {
        auto readlink = co_await serv.recv_req_readlink();
        if (not readlink) {
            co_return Unexpect{ readlink.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, readlink->path.data());

        auto buffer = Array<char, 1024>{};
        auto len    = ::readlink(readlink->path.data(), buffer.data(), buffer.size());
        if (len < 0) {
            auto err = log_status_from_errno(__func__, readlink->path, "failed to readlink");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Readlink{
            .target = Str{ buffer.begin(), static_cast<usize>(len) },
        });
    }

    AExpect<void> Server::handle_req_mknod(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_mkdir(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_unlink(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_rmdir(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_rename(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_truncate(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_read(rpc::Server& serv)
    {
        auto read = co_await serv.recv_req_read();
        if (not read) {
            co_return Unexpect{ read.error() };
        }
        spdlog::debug("{}: path={:?} size={}", __func__, read->path.data(), read->size);

        auto fd = ::open(read->path.data(), O_RDONLY);
        if (fd < 0) {
            auto err = log_status_from_errno(__func__, read->path, "failed to open file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        if (::lseek(fd, read->offset, SEEK_SET) < 0) {
            auto err = log_status_from_errno(__func__, read->path, "failed to seek file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        auto& buf = serv.buf();
        buf.resize(read->size);

        auto len = ::read(fd, buf.data(), buf.size());
        if (len < 0) {
            auto err = log_status_from_errno(__func__, read->path, "failed to read file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Read{
            .read = Span{ buf.begin(), static_cast<usize>(len) },
        });
    }

    AExpect<void> Server::handle_req_write(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_utimens(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }

    AExpect<void> Server::handle_req_copy_file_range(rpc::Server& serv)
    {
        co_return co_await serv.send_resp(rpc::Status::PermissionDenied);
    }
}
