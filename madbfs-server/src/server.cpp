#include "madbfs-server/server.hpp"
#include "madbfs-server/defer.hpp"    // DEFER macro

#include <madbfs-common/rpc.hpp>
#include <madbfs-common/util/overload.hpp>

#include <spdlog/spdlog.h>

#include <dirent.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{
    std::string err_msg(madbfs::Errc errc)
    {
        return std::make_error_code(errc).message();
    }

    madbfs::rpc::Status status_from_errno(madbfs::Str name, madbfs::Str path, madbfs::Str msg)
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
    AExpect<void> RequestHandler::dispatch()
    {
        auto procedure = co_await m_server.peek_req();
        if (not procedure) {
            spdlog::error("{}: failed to get param for procedure {}", __func__, err_msg(procedure.error()));
            co_return Expect<void>{};
        }
        spdlog::info("{}: accepted procedure [{}]", __func__, to_string(*procedure));

        auto handler = [&]<typename Req>(AExpect<Req> (rpc::Server::*recv)()) mutable -> AExpect<void> {
            auto req = co_await (m_server.*recv)();
            if (not req) {
                co_return Unexpect{ req.error() };
            }
            auto resp = handle_req(std::move(*req));
            co_return co_await m_server.send_resp(std::move(resp));
        };

        // clang-format off
        switch (*procedure) {
        case rpc::Procedure::Listdir:       co_return co_await handler(&rpc::Server::recv_req_listdir);
        case rpc::Procedure::Stat:          co_return co_await handler(&rpc::Server::recv_req_stat);
        case rpc::Procedure::Readlink:      co_return co_await handler(&rpc::Server::recv_req_readlink);
        case rpc::Procedure::Mknod:         co_return co_await handler(&rpc::Server::recv_req_mknod);
        case rpc::Procedure::Mkdir:         co_return co_await handler(&rpc::Server::recv_req_mkdir);
        case rpc::Procedure::Unlink:        co_return co_await handler(&rpc::Server::recv_req_unlink);
        case rpc::Procedure::Rmdir:         co_return co_await handler(&rpc::Server::recv_req_rmdir);
        case rpc::Procedure::Rename:        co_return co_await handler(&rpc::Server::recv_req_rename);
        case rpc::Procedure::Truncate:      co_return co_await handler(&rpc::Server::recv_req_truncate);
        case rpc::Procedure::Read:          co_return co_await handler(&rpc::Server::recv_req_read);
        case rpc::Procedure::Write:         co_return co_await handler(&rpc::Server::recv_req_write);
        case rpc::Procedure::Utimens:       co_return co_await handler(&rpc::Server::recv_req_utimens);
        case rpc::Procedure::CopyFileRange: co_return co_await handler(&rpc::Server::recv_req_copy_file_range);
        }
        // clang-format on

        spdlog::error("{}: invalid procedure with integral value {}", __func__, static_cast<u8>(*procedure));

        co_return Expect<void>{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Listdir req)
    {
        const auto& [path] = req;
        spdlog::debug("listdir: path={:?}", path.data());

        auto dir = ::opendir(path.data());
        if (dir == nullptr) {
            return status_from_errno(__func__, path, "failed to open dir");
        }

        DEFER {
            if (::closedir(dir) < 0) {
                status_from_errno(__func__, path, "failed to close dir");
            }
        };

        struct Slice
        {
            isize offset;
            usize size;
        };

        // WARN: invalidates strings and spans from argument
        auto& buf = m_server.buf();
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
                status_from_errno(__func__, name, "failed to stat file");
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

        return rpc::resp::Listdir{ .entries = std::move(entries) };
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Stat req)
    {
        const auto& [path] = req;
        spdlog::debug("stat: path={:?}", path.data());

        struct stat filestat = {};
        if (auto res = ::lstat(path.data(), &filestat); res < 0) {
            return status_from_errno(__func__, path, "failed to stat file");
        }

        return rpc::resp::Stat{
            .size  = filestat.st_size,
            .links = filestat.st_nlink,
            .mtime = filestat.st_mtim,
            .atime = filestat.st_atim,
            .ctime = filestat.st_ctim,
            .mode  = filestat.st_mode,
            .uid   = filestat.st_uid,
            .gid   = filestat.st_gid,
        };
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Readlink req)
    {
        const auto& [path] = req;
        spdlog::debug("readlink: path={:?}", path.data());

        // NOTE: can't use server's buffer as destination since using it will invalidate path.
        // PERF: since the buffer won't change anyway, making it static reduces memory usage
        thread_local static auto buffer = Array<char, PATH_MAX>{};

        auto len = ::readlink(path.data(), buffer.data(), buffer.size());
        if (len < 0) {
            return status_from_errno(__func__, path, "failed to readlink");
        }

        return rpc::resp::Readlink{ .target = Str{ buffer.begin(), static_cast<usize>(len) } };
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Mknod req)
    {
        const auto& [path, mode, dev] = req;
        spdlog::debug("mknod: path={:?}", path.data());

        if (::mknod(path.data(), mode, dev) < 0) {
            return status_from_errno(__func__, path, "failed to create file");
        }

        return rpc::resp::Mknod{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Mkdir req)
    {
        const auto& [path, mode] = req;
        spdlog::debug("mkdir: path={:?}", path.data());

        if (::mkdir(path.data(), mode) < 0) {
            return status_from_errno(__func__, path, "failed to create directory");
        }

        return rpc::resp::Mkdir{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Unlink req)
    {
        const auto& [path] = req;
        spdlog::debug("unlink: path={:?}", path.data());

        if (::unlink(path.data()) < 0) {
            return status_from_errno(__func__, path, "failed to remove file");
        }

        return rpc::resp::Unlink{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Rmdir req)
    {
        const auto& [path] = req;
        spdlog::debug("rmdir: path={:?}", path.data());

        if (::rmdir(path.data()) < 0) {
            return status_from_errno(__func__, path, "failed to remove directory");
        }

        return rpc::resp::Rmdir{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Rename req)
    {
        const auto& [from, to, flags] = req;
        spdlog::debug("rename: from={:?} -> to={:?} [flags={}]", from, to, flags);

        // NOTE: renameat2 is not exposed directly by Android's linux kernel apparently (or not supported).
        // workaround: https://stackoverflow.com/a/41655792/16506263 (https://lwn.net/Articles/655028/).

        // NOTE: paths are guaranteed to be absolute for both from and to, so the fds are not required since
        // they will be ignored. see man rename(2).

        // NOTE: This function will most likely return invalid argument anyway when given RENAME_EXCHANGE flag
        // since this operation is not widely supported.

        if (syscall(SYS_renameat2, 0, from.data(), 0, to.data(), flags) < 0) {
            return status_from_errno(__func__, from, "failed to rename file");
        }

        return rpc::resp::Rename{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Truncate req)
    {
        const auto& [path, size] = req;
        spdlog::debug("truncate: path={:?}", path.data());

        if (::truncate(path.data(), size) < 0) {
            return status_from_errno(__func__, path, "failed to truncate file");
        }

        return rpc::resp::Truncate{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Read req)
    {
        const auto& [path, offset, size] = req;
        spdlog::debug("read: path={:?} size={}", path.data(), size);

        auto fd = ::open(path.data(), O_RDONLY);
        if (fd < 0) {
            return status_from_errno(__func__, path, "failed to open file");
        }

        DEFER {
            if (::close(fd) < 0) {
                status_from_errno(__func__, path, "failed to close file");
            }
        };

        if (::lseek(fd, offset, SEEK_SET) < 0) {
            return status_from_errno(__func__, path, "failed to seek file");
        }

        // WARN: invalidates strings and spans from argument
        auto& buf = m_server.buf();
        buf.resize(size);

        auto len = ::read(fd, buf.data(), buf.size());
        if (len < 0) {
            return status_from_errno(__func__, path, "failed to read file");
        }

        return rpc::resp::Read{ .read = Span{ buf.begin(), static_cast<usize>(len) } };
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Write req)
    {
        const auto& [path, offset, in] = req;
        spdlog::debug("write: path={:?} size={}", path.data(), in.size());

        auto fd = ::open(path.data(), O_WRONLY);
        if (fd < 0) {
            return status_from_errno(__func__, path, "failed to open file");
        }

        DEFER {
            if (::close(fd) < 0) {
                status_from_errno(__func__, path, "failed to close file");
            }
        };

        if (::lseek(fd, offset, SEEK_SET) < 0) {
            return status_from_errno(__func__, path, "failed to seek file");
        }

        auto len = ::write(fd, in.data(), in.size());
        if (len < 0) {
            return status_from_errno(__func__, path, "failed to write file");
        }

        return rpc::resp::Write{ .size = static_cast<usize>(len) };
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Utimens req)
    {
        const auto& [path, atime, mtime] = req;
        spdlog::debug("utimens: path={:?}", path.data());

        auto times = Array{ atime, mtime };
        if (::utimensat(0, path.data(), times.data(), AT_SYMLINK_NOFOLLOW) < 0) {
            return status_from_errno(__func__, path, "failed to utimens file");
        }

        return rpc::resp::Utimens{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::CopyFileRange req)
    {
        const auto& [in, in_off, out, out_off, size] = req;
        spdlog::debug("copy_file_range: from={:?} -> to={:?}", in.data(), out.data());

        auto in_fd = ::open(in.data(), O_RDONLY);
        if (in_fd < 0) {
            return status_from_errno(__func__, in, "failed to open file");
        }
        DEFER {
            if (::close(in_fd) < 0) {
                status_from_errno(__func__, in, "failed to close file");
            }
        };

        if (::lseek(in_fd, in_off, SEEK_SET) < 0) {
            return status_from_errno(__func__, in, "failed to seek file");
        }

        auto out_fd = ::open(out.data(), O_RDONLY);
        if (out_fd < 0) {
            return status_from_errno(__func__, out, "failed to open file");
        }

        DEFER {
            if (::close(out_fd) < 0) {
                status_from_errno(__func__, out, "failed to close file");
            }
        };

        if (::lseek(out_fd, out_off, SEEK_SET) < 0) {
            return status_from_errno(__func__, out, "failed to seek file");
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

        if (len < 0) {
            return status_from_errno(__func__, out, "failed to seek file");
        }

        return rpc::resp::CopyFileRange{ .size = static_cast<usize>(copied) };
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
            async::spawn(exec, handle_connection(std::move(*sock)), async::detached);
        }

        co_return Expect<void>{};
    }

    void Server::stop()
    {
        m_running = false;
        m_acceptor.cancel();
        m_acceptor.close();
    }

    AExpect<void> Server::handle_connection(async::tcp::Socket sock)
    {
        auto ec   = std::error_code{};
        auto peer = sock.remote_endpoint(ec);
        if (ec) {
            spdlog::error("{}: failed to get endpoint: {}", __func__, ec.message());
        }
        spdlog::debug("{}: new connection from {}:{}", __func__, peer.address().to_string(), peer.port());

        auto handler = RequestHandler{ sock };
        co_return co_await handler.dispatch();
    }
}
