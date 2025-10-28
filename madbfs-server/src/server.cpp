#include "madbfs-server/server.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/rpc.hpp>
#include <madbfs-common/util/defer.hpp>
#include <madbfs-common/util/slice.hpp>

#include <dirent.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// android API changes: https://android.googlesource.com/platform/bionic/+/HEAD/docs/status.md

namespace
{
    madbfs::rpc::Status status_from_errno(
        madbfs::Str          name,
        madbfs::Str          path,
        madbfs::Str          msg,
        std::source_location loc = std::source_location::current()
    )
    {
        using madbfs::log::Level;
        using madbfs::log::log_loc;

        auto err = errno;
        log_loc(loc, Level::err, "{}: {} {:?}: {}", name, msg, path, strerror(err));
        return static_cast<madbfs::rpc::Status>(err);
    }
}

namespace madbfs::server
{
    RequestHandler::Response RequestHandler::handle_req(rpc::req::Listdir req)
    {
        const auto& [path] = req;
        log_d("listdir: path={:?}", path.data());

        auto dir = ::opendir(path.data());
        if (dir == nullptr) {
            return status_from_errno(__func__, path, "failed to open dir");
        }

        auto deferred = util::defer([=] {
            if (::closedir(dir) < 0) {
                std::ignore = status_from_errno(__func__, path, "failed to close dir");
            }
        });

        // WARN: invalidates strings and spans from argument
        auto& buf = m_buffer;
        buf.clear();

        auto slices = Vec<Pair<util::Slice, rpc::resp::Stat>>{};
        auto dirfd  = ::dirfd(dir);

        while (auto entry = ::readdir(dir)) {
            auto name = Str{ entry->d_name };
            if (name == "." or name == "..") {
                continue;
            }

            struct stat filestat = {};
            if (auto res = ::fstatat(dirfd, entry->d_name, &filestat, AT_SYMLINK_NOFOLLOW); res < 0) {
                std::ignore = status_from_errno(__func__, name, "failed to stat file");
                continue;
            }

            auto name_u8 = reinterpret_cast<const u8*>(name.data());
            auto off     = buf.size();

            buf.insert(buf.end(), name_u8, name_u8 + name.size());

            auto slice = util::Slice{ off, name.size() };
            auto stat  = rpc::resp::Stat{
                 .size  = static_cast<off_t>(filestat.st_size),
                 .links = static_cast<nlink_t>(filestat.st_nlink),
                 .mtime = filestat.st_mtim,
                 .atime = filestat.st_atim,
                 .ctime = filestat.st_ctim,
                 .mode  = static_cast<mode_t>(filestat.st_mode),
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
        log_d("stat: path={:?}", path.data());

        struct stat filestat = {};
        if (auto res = ::lstat(path.data(), &filestat); res < 0) {
            return status_from_errno(__func__, path, "failed to stat file");
        }

        return rpc::resp::Stat{
            .size  = static_cast<off_t>(filestat.st_size),
            .links = static_cast<nlink_t>(filestat.st_nlink),
            .mtime = filestat.st_mtim,
            .atime = filestat.st_atim,
            .ctime = filestat.st_ctim,
            .mode  = static_cast<mode_t>(filestat.st_mode),
            .uid   = filestat.st_uid,
            .gid   = filestat.st_gid,
        };
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Readlink req)
    {
        const auto& [path] = req;
        log_d("readlink: path={:?}", path.data());

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
        log_d("mknod: path={:?} mode={:#08o} dev={:#04x}", path.data(), mode, dev);

        if (::mknod(path.data(), mode, dev) < 0) {
            return status_from_errno(__func__, path, "failed to create file");
        }

        return rpc::resp::Mknod{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Mkdir req)
    {
        const auto& [path, mode] = req;
        log_d("mkdir: path={:?} mode={:#08o}", path.data(), mode);

        if (::mkdir(path.data(), mode) < 0) {
            return status_from_errno(__func__, path, "failed to create directory");
        }

        return rpc::resp::Mkdir{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Unlink req)
    {
        const auto& [path] = req;
        log_d("unlink: path={:?}", path.data());

        if (::unlink(path.data()) < 0) {
            return status_from_errno(__func__, path, "failed to remove file");
        }

        return rpc::resp::Unlink{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Rmdir req)
    {
        const auto& [path] = req;
        log_d("rmdir: path={:?}", path.data());

        if (::rmdir(path.data()) < 0) {
            return status_from_errno(__func__, path, "failed to remove directory");
        }

        return rpc::resp::Rmdir{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Rename req)
    {
        const auto& [from, to, flags] = req;
        log_d("rename: from={:?} -> to={:?} [flags={}]", from, to, flags);

        // paths are guaranteed to be absolute for both from and to, so the fds are not required since they
        // will be ignored. see man rename(2).

        // NOTE: renameat2 is only available from API level 30
        if (m_renameat2_impl) {
            auto res = syscall(SYS_renameat2, 0, from.data(), 0, to.data(), flags);
            if (res < 0 and errno == ENOSYS) {
                m_renameat2_impl = false;
                log_w("renameat2 syscall is not implemented, proceeding into fallback");
            } else if (res < 0) {
                return status_from_errno(__func__, from, "failed to rename file");
            } else {
                return rpc::resp::Rename{};
            }
        }

        // renameat don't take flags
        if (flags != 0) {
            return rpc::Status::invalid_argument;
        }

        if (::renameat(0, from.data(), 0, to.data()) < 0) {
            return status_from_errno(__func__, from, "failed to rename file");
        }

        return rpc::resp::Rename{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Truncate req)
    {
        const auto& [path, size] = req;
        log_d("truncate: path={:?} size={}", path.data(), size);

        if (::truncate(path.data(), size) < 0) {
            return status_from_errno(__func__, path, "failed to truncate file");
        }

        return rpc::resp::Truncate{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::Read req)
    {
        const auto& [path, offset, size] = req;
        log_d("read: path={:?} offset={} size={}", path.data(), offset, size);

        auto fd = ::open(path.data(), O_RDONLY);
        if (fd < 0) {
            return status_from_errno(__func__, path, "failed to open file");
        }

        auto deferred = util::defer([=] {
            if (::close(fd) < 0) {
                std::ignore = status_from_errno(__func__, path, "failed to close file");
            }
        });

        if (::lseek(fd, offset, SEEK_SET) < 0) {
            return status_from_errno(__func__, path, "failed to seek file");
        }

        // WARN: invalidates strings and spans from argument
        auto& buf = m_buffer;
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
        log_d("write: path={:?} offset={}, size={}", path.data(), offset, in.size());

        auto fd = ::open(path.data(), O_WRONLY);
        if (fd < 0) {
            return status_from_errno(__func__, path, "failed to open file");
        }

        auto deferred = util::defer([=] {
            if (::close(fd) < 0) {
                std::ignore = status_from_errno(__func__, path, "failed to close file");
            }
        });

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

        auto to_pair = [](timespec time) { return Pair{ time.tv_sec, time.tv_nsec }; };
        log_d("utimens: path={:?} atime={} mtime={}", path.data(), to_pair(atime), to_pair(mtime));

        auto times = Array{ atime, mtime };
        if (::utimensat(0, path.data(), times.data(), AT_SYMLINK_NOFOLLOW) < 0) {
            return status_from_errno(__func__, path, "failed to utimens file");
        }

        return rpc::resp::Utimens{};
    }

    RequestHandler::Response RequestHandler::handle_req(rpc::req::CopyFileRange req)
    {
        const auto& [in, in_off, out, out_off, size] = req;
        log_d("copy_file_range: from={:?} -> to={:?}", in.data(), out.data());

        auto in_fd = ::open(in.data(), O_RDONLY);
        if (in_fd < 0) {
            return status_from_errno(__func__, in, "failed to open file");
        }
        auto in_deferred = util::defer([=] {
            if (::close(in_fd) < 0) {
                std::ignore = status_from_errno(__func__, in, "failed to close file");
            }
        });

        if (::lseek(in_fd, in_off, SEEK_SET) < 0) {
            return status_from_errno(__func__, in, "failed to seek file");
        }

        auto out_fd = ::open(out.data(), O_WRONLY);
        if (out_fd < 0) {
            return status_from_errno(__func__, out, "failed to open file");
        }
        auto out_deferred = util::defer([=] {
            if (::close(out_fd) < 0) {
                std::ignore = status_from_errno(__func__, out, "failed to close file");
            }
        });

        if (::lseek(out_fd, out_off, SEEK_SET) < 0) {
            return status_from_errno(__func__, out, "failed to seek file");
        }

        // NOTE: copy_file_range syscall only available from API level 34
        if (m_copy_file_range_impl) {
            auto in_off_  = in_off;     // must be mutable
            auto out_off_ = out_off;    // must be mutable

            auto res = syscall(SYS_copy_file_range, in_fd, &in_off_, out_fd, &out_off_, size, 0);
            if (res < 0 and errno == ENOSYS) {
                m_copy_file_range_impl = false;
                log_w("copy_file_range syscall is not implemented, proceeding into fallback");
            } else if (res < 0) {
                return status_from_errno(__func__, out, "failed to seek file");
            } else {
                return rpc::resp::CopyFileRange{ .size = static_cast<usize>(res) };
            }
        }

        auto buffer = Array<char, 64 * 1024>{};
        auto copied = 0_usize;

        auto read    = 0_i64;
        auto written = 0_i64;

        while (copied < size and written >= 0) {
            if (read = ::read(in_fd, buffer.data(), buffer.size()); read <= 0) {
                break;
            }
            while (read > 0) {
                if (written = ::write(out_fd, buffer.data(), static_cast<usize>(read)); written < 0) {
                    goto end_copy;
                }
                read   -= written;
                copied += static_cast<usize>(written);
            }
        }
    end_copy:

        if (read < 0 or written < 0) {
            return status_from_errno(__func__, out, "failed to copy file");
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
        m_acceptor.listen(1);
    }

    Server::~Server()
    {
        if (m_running) {
            stop();
        }
    }

    AExpect<void> Server::run()
    {
        log_i("{}: madbfs-server version {}", __func__, MADBFS_VERSION_STRING);
        log_i("{}: launching tcp server on port: {}", __func__, m_acceptor.local_endpoint().port());

        m_running = true;

        while (m_running) {
            auto sock = co_await m_acceptor.async_accept();
            if (not sock) {
                log_e("{}: failed to accept connection: {}", __func__, sock.error().message());
                break;
            }

            if (auto res = co_await rpc::handshake(*sock); not res) {
                co_return Unexpect{ res.error() };
            }

            auto handler = [&](Vec<u8>& buf, rpc::Request req) -> Await<Var<rpc::Status, rpc::Response>> {
                auto handler  = RequestHandler{ buf };
                auto overload = [&](rpc::IsRequest auto&& req) { return handler.handle_req(std::move(req)); };
                co_return std::visit(std::move(overload), std::move(req));
            };

            auto rpc = rpc::Server{ std::move(*sock) };
            auto res = co_await rpc.listen(handler);
            if (not res) {
                log_e("{}: rpc::Server::listen return with an error: {}", __func__, err_msg(res.error()));
            }
        }

        co_return Expect<void>{};
    }

    void Server::stop()
    {
        m_running = false;
        m_acceptor.cancel();
        m_acceptor.close();
    }
}
