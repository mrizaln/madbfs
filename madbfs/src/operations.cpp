#include "madbfs/operations.hpp"

#include "madbfs/args.hpp"
#include "madbfs/madbfs.hpp"

#include "madbfs-common/log.hpp"

namespace
{
    /**
     * @brief Get application instance.
     */
    madbfs::Madbfs& get_data() noexcept
    {
        auto ctx = ::fuse_get_context()->private_data;
        assert(ctx != nullptr);
        return *static_cast<madbfs::Madbfs*>(ctx);
    }

    /**
     * @brief Interface sync code of FUSE with async code of madbfs on the tree access using future.
     *
     * @param fn The member function of `FileTree`.
     * @param args Arguments to be passed into the member function.
     *
     * @return The return value of the member function.
     */
    template <typename Ret, typename... Args>
    Ret invoke_tree(
        madbfs::Await<Ret> (madbfs::tree::FileTree::*fn)(Args...),
        std::type_identity_t<Args>... args
    ) noexcept
    {
        auto& data = get_data();
        auto& ctx  = data.async_ctx();
        auto& tree = data.tree();

        try {
            auto coro = (tree.*fn)(std::forward<Args>(args)...);
            return madbfs::async::block(ctx, std::move(coro));
        } catch (const std::exception& e) {
            madbfs::log_c("invoke_tree: exception occurred: {}", e.what());
        } catch (...) {
            madbfs::log_c("invoke_tree: unknown exception occurred");
        }
        return madbfs::Unexpect{ madbfs::Errc::io_error };
    }

    auto fuse_err(
        madbfs::Str          name,
        const char*          path,
        std::source_location loc = std::source_location::current()
    )
    {
        using madbfs::log::Level;
        auto log = [=]<typename... Args>(Level level, fmt::format_string<Args...>&& fmt, Args&&... args) {
            madbfs::log::log_loc(loc, level, std::move(fmt), std::forward<Args>(args)...);
        };

        return [=](std::errc err) {
            const auto errint = static_cast<int>(err);
            if (errint == 0) {
                return 0;
            }

            // filter out common errors
            switch (err) {
            case std::errc::no_such_file_or_directory:
            case std::errc::file_exists:
            case std::errc::not_a_directory:
            case std::errc::is_a_directory:
            case std::errc::directory_not_empty:
            case std::errc::too_many_symbolic_link_levels:
            case std::errc::permission_denied:
            case std::errc::read_only_file_system:
            case std::errc::filename_too_long:
            case std::errc::invalid_argument: {
                if (madbfs::log::get_level() == madbfs::log::Level::debug) {
                    const auto msg = std::make_error_code(err).message();
                    log(Level::warn, "{}: {:?} returned with error code [{}]: {}", name, path, errint, msg);
                }
            } break;
            default: {
                const auto msg = std::make_error_code(err).message();
                log(Level::err, "{}: {:?} returned with error code [{}]: {}", name, path, errint, msg);
            }
            }
            return -errint;
        };
    }
}

namespace madbfs::operations
{
    using tree::FileTree;

    void* init(fuse_conn_info* conn, fuse_config*) noexcept
    {
        if (conn->want & FUSE_CAP_ATOMIC_O_TRUNC) {
            auto msg = "fuse sets atomic O_TRUNC capability, but filesystem doesn't support it, disabling...";
            log_w("{}: {}", __func__, msg);
            conn->want &= ~static_cast<u32>(FUSE_CAP_ATOMIC_O_TRUNC);
        }

        auto* args = static_cast<args::ParsedOpt*>(::fuse_get_context()->private_data);
        assert(args != nullptr and "data should not be empty!");

        if (args->server and not args->server->is_absolute()) {
            log_c("{}: server path is not absolute when it should! ignoring", __func__);
            args->server.reset();
        }

        auto cache_size = args->cachesize * 1024 * 1024;
        auto page_size  = args->pagesize * 1024;
        auto max_pages  = cache_size / page_size;
        auto server     = args->server.transform(&std::filesystem::path::c_str).and_then(&path::create);
        auto ttl        = args->ttl < 0 ? std::nullopt : Opt<Seconds>{ args->ttl };
        auto timeout    = args->timeout < 1 ? std::nullopt : Opt<Seconds>{ args->timeout };

        return new Madbfs{
            server.transform(proj(&path::SemiPath::path)),
            args->port,
            page_size,
            max_pages,
            args->mount,
            ttl,
            timeout,
        };
    }

    void destroy(void* private_data) noexcept
    {
        auto* data = static_cast<Madbfs*>(private_data);
        assert(data != nullptr and "data should not be empty!");
        delete data;

        auto serial = ::getenv("ANDROID_SERIAL");
        if (serial != nullptr) {
            log_i("madbfs for device {} succesfully terminated", serial);
        } else [[unlikely]] {
            log_i("madbfs succesfully terminated");
        }

        // to force flushing remaining logs in queue
        log::shutdown();
    }

    i32 getattr(const char* path, struct stat* stbuf, [[maybe_unused]] fuse_file_info* fi) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        auto maybe_stat = ok_or(path::create(path), Errc::operation_not_supported).and_then([](path::Path p) {
            return invoke_tree(&FileTree::getattr, p);
        });
        if (not maybe_stat.has_value()) {
            return fuse_err(__func__, path)(maybe_stat.error());
        }

        const auto& stat = maybe_stat->get();

        std::memset(stbuf, 0, sizeof(struct stat));

        stbuf->st_ino     = static_cast<ino_t>(stat.id.inner());
        stbuf->st_mode    = stat.mode;
        stbuf->st_nlink   = stat.links;
        stbuf->st_uid     = stat.uid;
        stbuf->st_gid     = stat.gid;
        stbuf->st_size    = stat.size;
        stbuf->st_blksize = static_cast<blksize_t>(get_data().cache().page_size());
        stbuf->st_blocks  = (stbuf->st_size + 511) % 512;    // strictly in 512 B unit [read stat(3)]
        stbuf->st_atim    = stat.atime;
        stbuf->st_mtim    = stat.mtime;
        stbuf->st_ctim    = stat.ctime;

        return 0;
    }

    i32 readlink(const char* path, char* buf, size_t size) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](path::Path p) { return invoke_tree(&FileTree::readlink, p); })
            .and_then([&](Str target) -> Expect<void> {
                if (target.size() < 1) {
                    return Unexpect{ Errc::invalid_argument };
                }

                if (target[0] == '/') {    // absolute link
                    auto mount = get_data().mountpoint();
                    if (auto pathsize = mount.size() + target.size() + 1; pathsize > size) {
                        log_e("readlink: path size is too long: {} vs {}", size, pathsize);
                        return Unexpect{ Errc::filename_too_long };
                    }
                    std::memcpy(buf, mount.data(), mount.size());
                    std::memcpy(buf + mount.size(), target.data(), target.size());
                    buf[mount.size() + target.size()] = '\0';
                } else {
                    if (auto pathsize = target.size(); pathsize + 1 > size) {
                        log_e("readlink: path size is too long: {} vs {}", size, pathsize);
                        return Unexpect{ Errc::filename_too_long };
                    }
                    std::memcpy(buf, target.data(), target.size());
                    buf[target.size()] = '\0';
                }

                return {};
            })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 mknod(const char* path, mode_t mode, dev_t dev) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([=](path::Path p) { return invoke_tree(&FileTree::mknod, p, mode, dev); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 mkdir(const char* path, mode_t mode) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([=](path::Path p) { return invoke_tree(&FileTree::mkdir, p, mode | S_IFDIR); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 unlink(const char* path) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](path::Path p) { return invoke_tree(&FileTree::unlink, p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 rmdir(const char* path) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](path::Path p) { return invoke_tree(&FileTree::rmdir, p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    // see: man page of rename(2)
    i32 rename(const char* from, const char* to, u32 flags) noexcept
    {
        log_i("{}: {:?} -> {:?} [flags={}]", __func__, from, to, flags);

        auto from_path = path::create(from);
        auto to_path   = path::create(to);

        if (not from_path.has_value()) {
            return fuse_err(__func__, from)(Errc::operation_not_supported);
        }
        if (not to_path.has_value()) {
            return fuse_err(__func__, to)(Errc::operation_not_supported);
        }

        return invoke_tree(&FileTree::rename, *from_path, *to_path, flags)
            .transform_error(fuse_err(__func__, from))
            .error_or(0);
    }

    i32 truncate(const char* path, off_t size, [[maybe_unused]] fuse_file_info* fi) noexcept
    {
        log_i("{}: [size={}] {:?}", __func__, size, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](path::Path p) { return invoke_tree(&FileTree::truncate, p, size); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 open(const char* path, fuse_file_info* fi) noexcept
    {
        log_i("{}: {:?} [flags={:#08o}]", __func__, path, fi->flags);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](path::Path p) { return invoke_tree(&FileTree::open, p, fi->flags); })
            .transform([&](u64 fd) { fi->fh = fd; })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 read(const char* path, char* buf, size_t size, off_t offset, fuse_file_info* fi) noexcept
    {
        log_i("{}: [offset={}|size={}] {:?}", __func__, offset, size, path);

        auto res = ok_or(path::create(path), Errc::operation_not_supported).and_then([&](path::Path p) {
            return invoke_tree(&FileTree::read, p, fi->fh, { buf, size }, offset);
        });
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__, path)(res.error());
    }

    i32 write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi) noexcept
    {
        log_i("{}: [offset={}|size={}] {:?}", __func__, offset, size, path);

        auto res = ok_or(path::create(path), Errc::operation_not_supported).and_then([&](path::Path p) {
            return invoke_tree(&FileTree::write, p, fi->fh, { buf, size }, offset);
        });
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__, path)(res.error());
    }

    i32 flush(const char* path, fuse_file_info* fi) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](path::Path p) { return invoke_tree(&FileTree::flush, p, fi->fh); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 release(const char* path, fuse_file_info* fi) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](path::Path p) { return invoke_tree(&FileTree::release, p, fi->fh); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 readdir(
        const char*                         path,
        void*                               buf,
        fuse_fill_dir_t                     filler,
        [[maybe_unused]] off_t              offset,
        [[maybe_unused]] fuse_file_info*    fi,
        [[maybe_unused]] fuse_readdir_flags flags
    ) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        const auto fill = [&](const char* name) { filler(buf, name, nullptr, 0, FUSE_FILL_DIR_PLUS); };

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](path::Path p) { return invoke_tree(&FileTree::readdir, p, fill); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 access([[maybe_unused]] const char* path, [[maybe_unused]] i32 mask) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        // NOTE: empty

        return 0;
    }

    i32 utimens(const char* path, const timespec tv[2], [[maybe_unused]] fuse_file_info* fi) noexcept
    {
        log_i("{}: {:?}", __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](path::Path p) { return invoke_tree(&FileTree::utimens, p, tv[0], tv[1]); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    isize copy_file_range(
        const char*            in_path,
        struct fuse_file_info* in_fi,
        off_t                  in_off,
        const char*            out_path,
        struct fuse_file_info* out_fi,
        off_t                  out_off,
        size_t                 size,
        [[maybe_unused]] int   flags
    ) noexcept
    {
        log_i(
            "{}: [size={}] | {:?} [off={}] -> {:?} [off={}]",
            __func__,
            size,
            in_path,
            in_off,
            out_path,
            out_off
        );

        auto in  = path::create(in_path);
        auto out = path::create(out_path);

        if (not in) {
            return fuse_err(__func__, in_path)(Errc::operation_not_supported);
        } else if (not out) {
            return fuse_err(__func__, out_path)(Errc::operation_not_supported);
        }

        auto op  = &FileTree::copy_file_range;
        auto res = invoke_tree(op, *in, in_fi->fh, in_off, *out, out_fi->fh, out_off, size);
        return res ? static_cast<isize>(res.value()) : fuse_err(__func__, in_path)(res.error());
    }
}
