#include "adbfsm/adbfsm.hpp"
#include "adbfsm/args.hpp"
#include "adbfsm/log.hpp"

#include <fcntl.h>

#include <cassert>
#include <cstring>

namespace
{
    /**
     * @brief Create a temporary directory.
     * @return The path to the temporary directory.
     *
     * The created directory handling, including its removal, is up to the caller.
     */
    std::filesystem::path make_temp_dir()
    {
        char adbfsm_template[] = "/tmp/adbfsm-XXXXXX";
        auto temp              = ::mkdtemp(adbfsm_template);
        adbfsm::log_i({ "created temporary directory: {:?}" }, temp);
        return temp;
    }

    adbfsm::Adbfsm& get_data()
    {
        auto ctx = ::fuse_get_context()->private_data;
        assert(ctx != nullptr);
        return *static_cast<adbfsm::Adbfsm*>(ctx);
    }

    auto fuse_err(adbfsm::Str name)
    {
        return [name](std::errc err) {
            if (err != std::errc{}) {
                auto msg = std::make_error_code(err).message();
                adbfsm::log_e({ "{}: returned with error code [{}]: {}" }, name, static_cast<int>(err), msg);
            }
            return -static_cast<int>(err);
        };
    }
}

namespace adbfsm
{
    void* init(fuse_conn_info*, fuse_config*)
    {
        auto* args = static_cast<args::ParsedOpt*>(::fuse_get_context()->private_data);
        assert(args != nullptr and "data should not be empty!");

        auto temp       = make_temp_dir();
        auto connection = std::make_unique<data::Connection>();
        auto cache      = std::make_unique<data::Cache>(temp, args->m_cachesize * 1000 * 1000);

        auto* connection_ptr = connection.get();
        auto* cache_ptr      = cache.get();

        return new Adbfsm{
            .connection = std::move(connection),
            .cache      = std::move(cache),
            .tree       = adbfsm::tree::FileTree{ *connection_ptr, *cache_ptr },
            .cache_dir  = temp,
        };
    }

    void destroy(void* private_data)
    {
        auto* data = static_cast<Adbfsm*>(private_data);
        assert(data != nullptr and "data should not be empty!");

        auto temp = data->cache_dir;
        delete data;

        adbfsm::log_i({ "cleaned up temporary directory: {:?}" }, temp.c_str());
        std::filesystem::remove_all(temp);
    }

    i32 getattr(const char* path, struct stat* stbuf, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        auto maybe_stat = ok_or(path::create(path), std::errc::operation_not_supported).and_then([](auto p) {
            return get_data().tree.getattr(p);
        });
        if (not maybe_stat.has_value()) {
            return fuse_err(__func__)(maybe_stat.error());
        }

        const auto& stat = **maybe_stat;

        std::memset(stbuf, 0, sizeof(struct stat));

        stbuf->st_ino   = 1;    // fake inode number;
        stbuf->st_mode  = static_cast<__mode_t>(stat.mode);
        stbuf->st_nlink = static_cast<__nlink_t>(stat.links);
        stbuf->st_uid   = static_cast<__uid_t>(stat.uid);
        stbuf->st_gid   = static_cast<__gid_t>(stat.gid);

        switch (stbuf->st_mode & S_IFMT) {
        case S_IFBLK:    // TODO: implement parse_file_stat but for block and character devices
        case S_IFCHR: stbuf->st_size = 0; break;
        case S_IFREG: stbuf->st_size = stat.size; break;
        case S_IFSOCK:
        case S_IFIFO:
        case S_IFDIR:
        default: stbuf->st_size = 0;
        }

        stbuf->st_blksize = 512;
        stbuf->st_blocks  = (stbuf->st_size + 256) / 512;

        auto time = timespec{ .tv_sec = stat.mtime, .tv_nsec = 0 };

        stbuf->st_atim = time;
        stbuf->st_mtim = time;
        stbuf->st_ctim = time;

        return 0;
    }

    i32 readlink(const char* path, char* buf, size_t size)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([](path::Path p) { return get_data().tree.readlink(p); })
            .and_then([&](tree::Node* node) -> Expect<void> {
                auto path = node->build_path();    // this will emits absolute path, which we don't want
                if (path.size() - 1 < size) {
                    std::memcpy(buf, path.c_str() + 1, path.size());    // copy path without initial '/'

                    return {};
                } else {
                    log_e({ "readlink: path size is too long: {} vs {}" }, size, path.size());
                    return std::unexpected{ std::errc::filename_too_long };
                }
            })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 mknod(const char* path, [[maybe_unused]] mode_t mode, [[maybe_unused]] dev_t rdev)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([](path::Path p) { return get_data().tree.mknod(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 mkdir(const char* path, [[maybe_unused]] mode_t mode)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([](path::Path p) { return get_data().tree.mkdir(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 unlink(const char* path)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([](auto p) { return get_data().tree.unlink(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 rmdir(const char* path)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([](auto p) { return get_data().tree.rmdir(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    // TODO: handle flags
    i32 rename(const char* from, const char* to, [[maybe_unused]] u32 flags)
    {
        log_i({ "{}: {:?} -> {:?}" }, __func__, from, to);

        auto from_path = path::create(from);
        auto to_path   = path::create(to);

        if (not from_path.has_value()) {
            return fuse_err(__func__)(std::errc::operation_not_supported);
        }
        if (not to_path.has_value()) {
            return fuse_err(__func__)(std::errc::operation_not_supported);
        }

        return get_data().tree.rename(*from_path, *to_path).transform_error(fuse_err(__func__)).error_or(0);
    }

    i32 truncate(const char* path, off_t size, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree.truncate(p, size); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 open(const char* path, fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree.open(p, fi->flags); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 read(const char* path, char* buf, size_t size, off_t offset, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        auto res = ok_or(path::create(path), std::errc::operation_not_supported).and_then([&](auto p) {
            return get_data().tree.read(p, { buf, size }, offset);
        });
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__)(res.error());
    }

    i32 write(
        const char*                      path,
        const char*                      buf,
        size_t                           size,
        off_t                            offset,
        [[maybe_unused]] fuse_file_info* fi
    )
    {
        log_i({ "{}: {:?}" }, __func__, path);

        auto res = ok_or(path::create(path), std::errc::operation_not_supported).and_then([&](auto p) {
            return get_data().tree.write(p, { buf, size }, offset);
        });
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__)(res.error());
    }

    i32 flush(const char* path, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree.flush(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 release(const char* path, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree.release(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 readdir(
        const char*                         path,
        void*                               buf,
        fuse_fill_dir_t                     filler,
        [[maybe_unused]] off_t              offset,
        [[maybe_unused]] fuse_file_info*    fi,
        [[maybe_unused]] fuse_readdir_flags flags
    )
    {
        log_i({ "{}: {:?}" }, __func__, path);

        const auto fill = [&](const char* name) { filler(buf, name, nullptr, 0, FUSE_FILL_DIR_PLUS); };

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree.readdir(p, fill); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }

    i32 access([[maybe_unused]] const char* path, [[maybe_unused]] i32 mask)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        // NOTE: empty

        return 0;
    }

    i32 utimens(const char* path, [[maybe_unused]] const timespec tv[2], [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), std::errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree.utimens(p); })
            .transform_error(fuse_err(__func__))
            .error_or(0);
    }
}
