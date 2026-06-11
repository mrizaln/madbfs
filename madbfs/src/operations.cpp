#include "madbfs/operations.hpp"

#include "madbfs/args.hpp"
#include "madbfs/madbfs.hpp"

#include <madbfs-common/log.hpp>

using namespace madbfs;

// helper functions/classes
namespace
{
    /**
     * @brief Get application instance.
     */
    Madbfs& get_data() noexcept
    {
        auto ctx = ::fuse_get_context()->private_data;
        assert(ctx != nullptr);
        return *static_cast<Madbfs*>(ctx);
    }

    /**
     * @brief Interface sync code of FUSE with async code of madbfs on the tree access using future.
     *
     * @param fn The member function of `Filesystem`.
     * @param args Arguments to be passed into the member function.
     *
     * @return The return value of the member function.
     */
    template <typename Ret, typename... Args>
    Ret invoke_fs(Await<Ret> (Filesystem::*fn)(Args...), std::type_identity_t<Args>... args) noexcept
    {
        auto& data = get_data();
        auto& ctx  = data.ctx();
        auto& fs   = data.fs();

        try {
            auto coro = (fs.*fn)(std::forward<Args>(args)...);
            return async::block(ctx, std::move(coro));
        } catch (const std::exception& e) {
            log_c(__func__, "exception occurred: {}", e.what());
        } catch (...) {
            log_c(__func__, "unknown exception occurred");
        }
        return Unexpect{ Errc::io_error };
    }

    /**
     * @brief Factory function for lambda that convert `std::errc` into its integer value and logs it.
     *
     * @param name Log name/prefix.
     * @param path Path being operated on.
     * @param loc Location of the function call.
     *
     * @return Lambda that transforms error into integer and logs them.
     */
    auto fuse_err(
        const char*          name,
        const char*          path,
        std::source_location loc = std::source_location::current()
    )
    {
        using log::Level;
        auto log = [=]<typename... Args>(Level level, fmt::format_string<Args...>&& fmt, Args&&... args) {
            log::log_loc_named(loc, level, name, std::move(fmt), std::forward<Args>(args)...);
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
                if (log::get_level() <= log::Level::debug) {
                    const auto& msg = err_msg(err);
                    log(Level::warn, "{:?} returned with error code [{}]: {}", path, errint, msg);
                }
            } break;
            default: {
                const auto& msg = err_msg(err);
                log(Level::err, "{:?} returned with error code [{}]: {}", path, errint, msg);
            }
            }
            return -errint;
        };
    }

    /**
     * @brief Resolve then store symlink target into buf.
     *
     * @param buf Output buffer.
     * @param target Symlink target.
     *
     * @return Nothing if success Errc if failure.
     *
     * This function resolves the symlink by prepending the mountpoint to the target if the symlink target is
     * absolute. It only stores the target if the target is relative.
     */
    Expect<void> resolve_symlink(Span<char> buf, Str target)
    {
        using namespace madbfs;

        if (target.size() < 1) {
            return Unexpect{ Errc::invalid_argument };
        }

        // custom root may break symlinks since the path pointed by the link might be unreachable, do I need
        // to handle this special case? currently I'm too lazy to think, so I'll just let it be.

        if (target[0] == '/') {    // absolute link
            auto mount = get_data().mountpoint();
            if (auto pathsize = mount.size() + target.size() + 1; pathsize > buf.size()) {
                log_e(__func__, "path size is too long: {} vs {}", buf.size(), pathsize);
                return Unexpect{ Errc::filename_too_long };
            }
            std::memcpy(buf.data(), mount.data(), mount.size());
            std::memcpy(buf.data() + mount.size(), target.data(), target.size());
            buf[mount.size() + target.size()] = '\0';
        } else {
            if (auto pathsize = target.size(); pathsize + 1 > buf.size()) {
                log_e(__func__, "path size is too long: {} vs {}", buf.size(), pathsize);
                return Unexpect{ Errc::filename_too_long };
            }
            std::memcpy(buf.data(), target.data(), target.size());
            buf[target.size()] = '\0';
        }

        return {};
    }
}

// operations.hpp impl
namespace madbfs::operations
{
    void* init(fuse_conn_info* conn, fuse_config*) noexcept
    {
        if (conn->want & FUSE_CAP_ATOMIC_O_TRUNC) {
            log_w(
                __func__,
                "fuse sets atomic O_TRUNC capability, but filesystem doesn't support it, disabling..."
            );
            conn->want &= ~static_cast<u32>(FUSE_CAP_ATOMIC_O_TRUNC);
        }

        auto* args = static_cast<args::ParsedOpt*>(::fuse_get_context()->private_data);
        assert(args != nullptr and "data should not be empty!");

        auto caching = args->caching.transform([](auto& c) {
            auto page_size = c.pagesize * 1024;
            return Caching{ .page_size = page_size, .max_pages = (c.cachesize * 1024 * 1024) / page_size };
        });

        auto ttl     = args->ttl < 1 ? std::nullopt : Opt<Seconds>{ args->ttl };
        auto timeout = args->timeout < 1 ? std::nullopt : Opt<Seconds>{ args->timeout };
        auto fuse    = ::fuse_get_context()->fuse;

        return new Madbfs{ fuse, args->connection, caching, args->root, args->mount, ttl, timeout };
    }

    void destroy(void* private_data) noexcept
    {
        auto* data = static_cast<Madbfs*>(private_data);
        assert(data != nullptr and "data should not be empty!");
        delete data;

        if (const auto* serial = ::getenv("ANDROID_SERIAL"); serial != nullptr) {
            log_i(__func__, "madbfs for device {} successfully terminated", serial);
        } else [[unlikely]] {
            log_i(__func__, "madbfs successfully terminated");
        }

        // to force flushing remaining logs in queue
        log::shutdown();
    }

    i32 getattr(const char* path, struct stat* stbuf, [[maybe_unused]] fuse_file_info* fi) noexcept
    {
        log_i(__func__, "{:?}", path);

        auto named_stat = get_data().create_path(path).and_then([](path::Path p) {
            return invoke_fs(&Filesystem::getattr, p);
        });
        if (not named_stat.has_value()) {
            return fuse_err(__func__, path)(named_stat.error());
        }

        const auto& [id, stat] = *named_stat;

        std::memset(stbuf, 0, sizeof(struct stat));

        const auto default_page_size = 64 * 1024;    // use minimum page size
        auto page_size = get_data().fs().cache().transform(&Cache::page_size).value_or(default_page_size);

        stbuf->st_ino     = static_cast<ino_t>(id.inner());
        stbuf->st_mode    = stat.mode;
        stbuf->st_nlink   = stat.links;
        stbuf->st_uid     = stat.uid;
        stbuf->st_gid     = stat.gid;
        stbuf->st_size    = stat.size;
        stbuf->st_blksize = static_cast<blksize_t>(page_size);
        stbuf->st_blocks  = (stbuf->st_size + 511) / 512;    // strictly in 512 B units [read stat(3)]
        stbuf->st_atim    = stat.atime;
        stbuf->st_mtim    = stat.mtime;
        stbuf->st_ctim    = stat.ctime;

        return 0;
    }

    i32 readlink(const char* path, char* buf, size_t size) noexcept
    {
        log_i(__func__, "{:?}", path);

        return get_data()
            .create_path(path)
            .and_then([](path::Path p) { return invoke_fs(&Filesystem::readlink, p); })
            .and_then([&](Str target) { return resolve_symlink({ buf, size }, target); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 mknod(const char* path, mode_t mode, dev_t dev) noexcept
    {
        log_i(__func__, "{:?}", path);

        return get_data()
            .create_path(path)
            .and_then([=](path::Path p) { return invoke_fs(&Filesystem::mknod, p, mode, dev); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 mkdir(const char* path, mode_t mode) noexcept
    {
        log_i(__func__, "{:?}", path);

        return get_data()
            .create_path(path)
            .and_then([=](path::Path p) { return invoke_fs(&Filesystem::mkdir, p, mode | S_IFDIR); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 unlink(const char* path) noexcept
    {
        log_i(__func__, "{:?}", path);

        return get_data()
            .create_path(path)
            .and_then([](path::Path p) { return invoke_fs(&Filesystem::unlink, p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 rmdir(const char* path) noexcept
    {
        log_i(__func__, "{:?}", path);

        return get_data()
            .create_path(path)
            .and_then([](path::Path p) { return invoke_fs(&Filesystem::rmdir, p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    // see: man page of rename(2)
    i32 rename(const char* from, const char* to, u32 flags) noexcept
    {
        log_i(__func__, "{:?} -> {:?} [flags={}]", from, to, flags);

        return get_data()
            .create_path2(from, to)
            .and_then([&](auto p) { return invoke_fs(&Filesystem::rename, p[0], p[1], flags); })
            .transform_error(fuse_err(__func__, from))
            .error_or(0);
    }

    i32 truncate(const char* path, off_t size, [[maybe_unused]] fuse_file_info* fi) noexcept
    {
        log_i(__func__, "[size={}] {:?}", size, path);

        return get_data()
            .create_path(path)
            .and_then([&](path::Path p) { return invoke_fs(&Filesystem::truncate, p, size); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 open(const char* path, fuse_file_info* fi) noexcept
    {
        log_i(__func__, "{:?} [flags={:#08o}]", path, fi->flags);

        return get_data()
            .create_path(path)
            .and_then([&](path::Path p) { return invoke_fs(&Filesystem::open, p, fi->flags); })
            .transform([&](u64 fd) { fi->fh = fd; })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 read(const char* path, char* buf, size_t size, off_t offset, fuse_file_info* fi) noexcept
    {
        log_i(__func__, "[offset={}|size={}] {:?}", offset, size, path);

        auto res = invoke_fs(&Filesystem::read, fi->fh, { buf, size }, offset);
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__, path)(res.error());
    }

    i32 write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi) noexcept
    {
        log_i(__func__, "[offset={}|size={}] {:?}", offset, size, path);

        auto res = invoke_fs(&Filesystem::write, fi->fh, { buf, size }, offset);
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__, path)(res.error());
    }

    i32 flush(const char* path, fuse_file_info* fi) noexcept
    {
        log_i(__func__, "{:?}", path);

        auto res = invoke_fs(&Filesystem::flush, fi->fh);
        return res.transform_error(fuse_err(__func__, path)).error_or(0);
    }

    i32 release(const char* path, fuse_file_info* fi) noexcept
    {
        log_i(__func__, "{:?}", path);

        auto res = invoke_fs(&Filesystem::release, fi->fh);
        return res.transform_error(fuse_err(__func__, path)).error_or(0);
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
        log_i(__func__, "{:?}", path);

        const auto fill = [&](const char* name) { filler(buf, name, nullptr, 0, FUSE_FILL_DIR_PLUS); };

        return get_data()
            .create_path(path)
            .and_then([&](path::Path p) { return invoke_fs(&Filesystem::readdir, p, fill); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 access([[maybe_unused]] const char* path, [[maybe_unused]] i32 mask) noexcept
    {
        return 0;    // NOTE: empty
    }

    i32 utimens(const char* path, const timespec tv[2], [[maybe_unused]] fuse_file_info* fi) noexcept
    {
        log_i(__func__, "{:?}", path);

        return get_data()
            .create_path(path)
            .and_then([&](path::Path p) { return invoke_fs(&Filesystem::utimens, p, tv[0], tv[1]); })
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
            __func__, "[size={}] | {:?} [off={}] -> {:?} [off={}]", size, in_path, in_off, out_path, out_off
        );

        auto res = get_data().create_path2(in_path, out_path).and_then([&](auto p) {
            auto op = &Filesystem::copy_file_range;
            return invoke_fs(op, p[0], in_fi->fh, in_off, p[1], out_fi->fh, out_off, size);
        });
        return res ? static_cast<isize>(res.value()) : fuse_err(__func__, in_path)(res.error());
    }
}
