#include "madbfs-server/server.hpp"

#include <madbfs-common/rpc.hpp>
#include <madbfs-common/util/overload.hpp>

#include <spdlog/spdlog.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    std::string err_msg(madbfs::Errc errc)
    {
        return std::make_error_code(errc).message();
    }

    madbfs::rpc::Status log_status_from_errno(madbfs::Str name, madbfs::Str path, madbfs::Str msg)
    {
        auto err = errno;
        spdlog::error("{}: {} {:?}: {}", name, msg, path, strerror(err));
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
        spdlog::info("{}: accepted procedure [{}]", __func__, to_string(*procedure));

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

        struct Slice
        {
            isize offset;
            usize size;
        };

        auto& buf = serv.buf();
        buf.clear();

        auto slices = Vec<Pair<Slice, rpc::resp::Stat>>{};
        auto dirfd  = ::dirfd(dir);

        while (auto entry = ::readdir(dir)) {
            auto name = Str{ entry->d_name };
            if (name == "." or name == "..") {
                continue;
            }

            struct stat filestat = {};
            if (auto res = ::fstatat(dirfd, entry->d_name, &filestat, AT_SYMLINK_NOFOLLOW); res < 0) {
                log_status_from_errno(__func__, name, "failed to stat file");
                continue;
            }

            auto name_u8 = reinterpret_cast<const u8*>(name.data());
            auto off     = buf.size();

            buf.insert(buf.end(), name_u8, name_u8 + name.size());

            auto slice = Slice{ static_cast<isize>(off), name.size() };
            auto stat  = rpc::resp::Stat{
                 .size  = filestat.st_size,
                 .links = filestat.st_nlink,
                 .mtime = filestat.st_mtim,
                 .atime = filestat.st_atim,
                 .ctime = filestat.st_ctim,
                 .mode  = filestat.st_mode,
                 .uid   = filestat.st_uid,
                 .gid   = filestat.st_gid,
            };

            slices.emplace_back(std::move(slice), std::move(stat));
        }

        auto entries = Vec<Pair<Str, rpc::resp::Stat>>{};
        entries.reserve(slices.size());

        for (auto&& [slice, stat] : slices) {
            auto name = Str{ reinterpret_cast<const char*>(buf.data()) + slice.offset, slice.size };
            entries.emplace_back(std::move(name), std::move(stat));
        }

        if (::closedir(dir) < 0) {
            log_status_from_errno(__func__, listdir->path, "failed to close dir");
        }

        co_return co_await serv.send_resp(rpc::resp::Listdir{ .entries = std::move(entries) });
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
        auto mknod = co_await serv.recv_req_mknod();
        if (not mknod) {
            co_return Unexpect{ mknod.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, mknod->path.data());

        if (::mknod(mknod->path.data(), S_IFREG, 0) < 0) {
            auto err = log_status_from_errno(__func__, mknod->path, "failed to create file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Mknod{});
    }

    AExpect<void> Server::handle_req_mkdir(rpc::Server& serv)
    {
        constexpr mode_t default_directory_mode = 040770;

        auto mkdir = co_await serv.recv_req_mkdir();
        if (not mkdir) {
            co_return Unexpect{ mkdir.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, mkdir->path.data());

        if (::mkdir(mkdir->path.data(), default_directory_mode) < 0) {
            auto err = log_status_from_errno(__func__, mkdir->path, "failed to create directory");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Mkdir{});
    }

    AExpect<void> Server::handle_req_unlink(rpc::Server& serv)
    {
        auto unlink = co_await serv.recv_req_unlink();
        if (not unlink) {
            co_return Unexpect{ unlink.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, unlink->path.data());

        if (::unlink(unlink->path.data()) < 0) {
            auto err = log_status_from_errno(__func__, unlink->path, "failed to remove file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Unlink{});
    }

    AExpect<void> Server::handle_req_rmdir(rpc::Server& serv)
    {
        auto rmdir = co_await serv.recv_req_rmdir();
        if (not rmdir) {
            co_return Unexpect{ rmdir.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, rmdir->path.data());

        if (::rmdir(rmdir->path.data()) < 0) {
            auto err = log_status_from_errno(__func__, rmdir->path, "failed to remove directory");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Rmdir{});
    }

    AExpect<void> Server::handle_req_rename(rpc::Server& serv)
    {
        auto rename = co_await serv.recv_req_rename();
        if (not rename) {
            co_return Unexpect{ rename.error() };
        }
        spdlog::debug("{}: from={:?} -> to={:?}", __func__, rename->from.data(), rename->to.data());

        if (::rename(rename->from.data(), rename->to.data()) < 0) {
            auto err = log_status_from_errno(__func__, rename->from, "failed to rename file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Rename{});
    }

    AExpect<void> Server::handle_req_truncate(rpc::Server& serv)
    {
        auto truncate = co_await serv.recv_req_truncate();
        if (not truncate) {
            co_return Unexpect{ truncate.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, truncate->path.data());

        if (::truncate(truncate->path.data(), truncate->size) < 0) {
            auto err = log_status_from_errno(__func__, truncate->path, "failed to truncate file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Truncate{});
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

        if (::close(fd) < 0) {
            log_status_from_errno(__func__, read->path, "failed to close file");
        }

        co_return co_await serv.send_resp(rpc::resp::Read{
            .read = Span{ buf.begin(), static_cast<usize>(len) },
        });
    }

    AExpect<void> Server::handle_req_write(rpc::Server& serv)
    {
        auto write = co_await serv.recv_req_write();
        if (not write) {
            co_return Unexpect{ write.error() };
        }
        spdlog::debug("{}: path={:?} size={}", __func__, write->path.data(), write->in.size());

        auto fd = ::open(write->path.data(), O_WRONLY);
        if (fd < 0) {
            auto err = log_status_from_errno(__func__, write->path, "failed to open file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        if (::lseek(fd, write->offset, SEEK_SET) < 0) {
            auto err = log_status_from_errno(__func__, write->path, "failed to seek file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        auto len = ::write(fd, write->in.data(), write->in.size());
        if (len < 0) {
            auto err = log_status_from_errno(__func__, write->path, "failed to write file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        if (::close(fd) < 0) {
            log_status_from_errno(__func__, write->path, "failed to close file");
        }

        co_return co_await serv.send_resp(rpc::resp::Write{ .size = static_cast<usize>(len) });
    }

    AExpect<void> Server::handle_req_utimens(rpc::Server& serv)
    {
        auto utimens = co_await serv.recv_req_utimens();
        if (not utimens) {
            co_return Unexpect{ utimens.error() };
        }
        spdlog::debug("{}: path={:?}", __func__, utimens->path.data());

        auto times = Array{ utimens->atime, utimens->mtime };
        if (::utimensat(0, utimens->path.data(), times.data(), AT_SYMLINK_NOFOLLOW) < 0) {
            auto err = log_status_from_errno(__func__, utimens->path, "failed to utimens file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::Utimens{});
    }

    AExpect<void> Server::handle_req_copy_file_range(rpc::Server& serv)
    {
        auto copy_file_range = co_await serv.recv_req_copy_file_range();
        if (not copy_file_range) {
            co_return Unexpect{ copy_file_range.error() };
        }

        auto [in, in_off, out, out_off, size] = *copy_file_range;
        spdlog::debug("{}: from={:?} -> to={:?}", __func__, in.data(), out.data());

        auto in_fd = ::open(in.data(), O_RDONLY);
        if (in_fd < 0) {
            auto err = log_status_from_errno(__func__, in, "failed to open file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        if (::lseek(in_fd, in_off, SEEK_SET) < 0) {
            auto err = log_status_from_errno(__func__, in, "failed to seek file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        auto out_fd = ::open(out.data(), O_RDONLY);
        if (out_fd < 0) {
            auto err = log_status_from_errno(__func__, out, "failed to open file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        if (::lseek(out_fd, out_off, SEEK_SET) < 0) {
            auto err = log_status_from_errno(__func__, out, "failed to seek file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        auto buffer = String(256 * 1024, '\0');

        auto copied = 0_i64;
        auto len    = 0_i64;

        while (len > 0) {
            if (len = ::read(in_fd, buffer.data(), buffer.size()); len <= 0) {
                break;
            }
            if (len = ::write(out_fd, buffer.data(), static_cast<usize>(len)); len < 0) {
                break;
            }
            copied += len;
        }

        if (::close(in_fd) < 0) {
            log_status_from_errno(__func__, in, "failed to close file");
        }

        if (::close(out_fd) < 0) {
            log_status_from_errno(__func__, out, "failed to close file");
        }

        if (len < 0) {
            auto err = log_status_from_errno(__func__, out, "failed to seek file");
            co_return co_await serv.send_resp(static_cast<rpc::Status>(err));
        }

        co_return co_await serv.send_resp(rpc::resp::CopyFileRange{ .size = static_cast<usize>(copied) });
    }
}
