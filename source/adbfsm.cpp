#include "adbfsm/adbfsm.hpp"
#include "adbfsm/args.hpp"
#include "adbfsm/data/ipc.hpp"
#include "adbfsm/log.hpp"
#include "adbfsm/util/overload.hpp"

#include <fcntl.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cstring>

namespace
{

    adbfsm::Adbfsm& get_data()
    {
        auto ctx = ::fuse_get_context()->private_data;
        assert(ctx != nullptr);
        return *static_cast<adbfsm::Adbfsm*>(ctx);
    }

    auto fuse_err(adbfsm::Str name, const char* path)
    {
        return [=](std::errc err) {
            const auto errint = static_cast<int>(err);
            if (errint == 0) {
                return 0;
            }

            switch (err) {
            // filter out common errors
            case std::errc::no_such_file_or_directory:
            case std::errc::file_exists:
            case std::errc::not_a_directory:
            case std::errc::is_a_directory:
            case std::errc::directory_not_empty:
            case std::errc::too_many_symbolic_link_levels:
            case std::errc::permission_denied:
            case std::errc::read_only_file_system:
            case std::errc::filename_too_long:
            case std::errc::invalid_argument:    //
                return -errint;

            default:
                const auto msg = std::make_error_code(err).message();
                adbfsm::log_e({ "{}: {:?} returned with error code [{}]: {}" }, name, path, errint, msg);
                return -errint;
            }
        };
    }
}

namespace adbfsm
{
    Adbfsm::Adbfsm(Uniq<data::IConnection> connection, Uniq<data::Cache> cache)
        : m_connection{ std::move(connection) }
        , m_cache{ std::move(cache) }
        , m_tree{ *m_connection, *m_cache }
    {
        auto ipc = data::Ipc::create();
        if (not ipc.has_value()) {
            const auto msg = std::make_error_code(ipc.error()).message();
            log_e({ "Adbfsm: failed to initialize ipc: {}" }, msg);
            return;
        }

        const auto path = ipc->path();
        log_i({ "Adbfsm: succesfully created ipc: {}" }, path.fullpath());
        m_ipc = std::move(*ipc);

        m_ipc->launch([this](data::ipc::Op op) { return ipc_handler(op); });
    }

    nlohmann::json Adbfsm::ipc_handler(data::ipc::Op op)
    {
        namespace ipc = data::ipc;

        constexpr usize lowest_page_size  = 64 * 1024;
        constexpr usize highest_page_size = 4 * 1024 * 1024;
        constexpr usize lowest_max_pages  = 128;

        auto overload = util::Overload{
            [&](ipc::Help) {
                return nlohmann::json("not implemented yet :P");    // TODO: implement
            },
            [&](ipc::InvalidateCache) {
                m_cache->invalidate();
                return nlohmann::json{};
            },
            [&](ipc::SetPageSize size) {
                auto old_size = m_cache->page_size();
                auto new_size = std::bit_ceil(size.kib * 1024);
                new_size      = std::clamp(new_size, lowest_page_size, highest_page_size);
                m_cache->set_page_size(new_size);

                auto old_max = m_cache->max_pages();
                auto new_max = std::bit_ceil(old_max * old_size / new_size);
                new_max      = std::max(new_max, lowest_max_pages);
                m_cache->set_max_pages(new_max);

                auto json              = nlohmann::json{};
                json["old_page_size"]  = old_size / 1024;
                json["old_cache_size"] = old_max * old_size / 1024 / 1024;
                json["new_page_size"]  = new_size / 1024;
                json["new_cache_size"] = new_max * new_size / 1024 / 1024;
                return json;
            },
            [&](ipc::GetPageSize) {
                auto size = m_cache->page_size();
                return nlohmann::json(size);
            },
            [&](ipc::SetCacheSize size) {
                auto page    = m_cache->page_size();
                auto old_max = m_cache->max_pages();
                auto new_max = std::bit_ceil(size.mib * 1024 * 1024 / page);
                new_max      = std::max(new_max, lowest_max_pages);
                m_cache->set_max_pages(new_max);

                auto json              = nlohmann::json{};
                json["old_cache_size"] = old_max * page / 1024 / 1024;
                json["new_cache_size"] = new_max * page / 1024 / 1024;
                return json;
            },
            [&](ipc::GetCacheSize) {
                auto page      = m_cache->page_size();
                auto num_pages = m_cache->max_pages();
                return nlohmann::json(page * num_pages / 1024 / 1024);
            },
        };
        return std::visit(overload, op);
    }

    void* init(fuse_conn_info*, fuse_config*)
    {
        auto* args = static_cast<args::ParsedOpt*>(::fuse_get_context()->private_data);
        assert(args != nullptr and "data should not be empty!");

        auto cache_size = args->cachesize * 1024 * 1024;
        auto page_size  = args->pagesize * 1024;
        auto max_pages  = cache_size / page_size;

        return new Adbfsm{
            std::make_unique<data::Connection>(page_size),
            std::make_unique<data::Cache>(page_size, max_pages),
        };
    }

    void destroy(void* private_data)
    {
        auto* data = static_cast<Adbfsm*>(private_data);
        assert(data != nullptr and "data should not be empty!");
        delete data;

        auto serial = ::getenv("ANDROID_SERIAL");
        if (serial != nullptr) {
            log_i({ "adbfsm for device {} succesfully terminated" }, serial);
        } else [[unlikely]] {
            log_i({ "adbfsm succesfully terminated" });
        }
    }

    i32 getattr(const char* path, struct stat* stbuf, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        auto maybe_stat = ok_or(path::create(path), Errc::operation_not_supported).and_then([](auto p) {
            return get_data().tree().getattr(p);
        });
        if (not maybe_stat.has_value()) {
            return fuse_err(__func__, path)(maybe_stat.error());
        }

        const auto& stat = maybe_stat->get();

        std::memset(stbuf, 0, sizeof(struct stat));

        stbuf->st_ino   = static_cast<ino_t>(stat.id.inner());
        stbuf->st_mode  = stat.mode;
        stbuf->st_nlink = stat.links;
        stbuf->st_uid   = stat.uid;
        stbuf->st_gid   = stat.gid;

        switch (stbuf->st_mode & S_IFMT) {
        case S_IFBLK:
        case S_IFCHR: stbuf->st_size = 0; break;
        case S_IFREG: stbuf->st_size = stat.size; break;
        case S_IFSOCK:
        case S_IFIFO:
        case S_IFDIR:
        default: stbuf->st_size = 0;
        }

        stbuf->st_blksize = static_cast<blksize_t>(get_data().cache().page_size());
        stbuf->st_blocks  = stbuf->st_size / stbuf->st_blksize + (stbuf->st_size % stbuf->st_blksize != 0);

        auto time = timespec{ .tv_sec = stat.mtime, .tv_nsec = 0 };

        stbuf->st_atim = time;
        stbuf->st_mtim = time;
        stbuf->st_ctim = time;

        return 0;
    }

    i32 readlink(const char* path, char* buf, size_t size)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](path::Path p) { return get_data().tree().readlink(p); })
            .and_then([&](tree::Node& node) -> Expect<void> {
                auto target_buf = node.build_path();    // this will emits absolute path, which we don't want
                auto target     = target_buf.as_path();
                if (auto pathsize = target.fullpath().size(); pathsize - 1 < size) {
                    std::memcpy(buf, target.fullpath().data() + 1, pathsize);    // copy without initial '/'
                    return {};
                } else {
                    log_e({ "readlink: path size is too long: {} vs {}" }, size, pathsize);
                    return Unexpect{ Errc::filename_too_long };
                }
            })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 mknod(const char* path, [[maybe_unused]] mode_t mode, [[maybe_unused]] dev_t rdev)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](path::Path p) { return get_data().tree().mknod(p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 mkdir(const char* path, [[maybe_unused]] mode_t mode)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](path::Path p) { return get_data().tree().mkdir(p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 unlink(const char* path)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](auto p) { return get_data().tree().unlink(p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 rmdir(const char* path)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([](auto p) { return get_data().tree().rmdir(p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    // TODO: handle flags
    i32 rename(const char* from, const char* to, [[maybe_unused]] u32 flags)
    {
        log_i({ "{}: {:?} -> {:?}" }, __func__, from, to);

        auto from_path = path::create(from);
        auto to_path   = path::create(to);

        if (not from_path.has_value()) {
            return fuse_err(__func__, from)(Errc::operation_not_supported);
        }
        if (not to_path.has_value()) {
            return fuse_err(__func__, to)(Errc::operation_not_supported);
        }

        return get_data()
            .tree()
            .rename(*from_path, *to_path)
            .transform_error(fuse_err(__func__, from))
            .error_or(0);
    }

    i32 truncate(const char* path, off_t size, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: [size={}] {:?}" }, __func__, size, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree().truncate(p, size); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 open(const char* path, fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree().open(p, fi->flags); })
            .transform([&](auto fd) { fi->fh = fd; })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 read(const char* path, char* buf, size_t size, off_t offset, [[maybe_unused]] fuse_file_info* fi)
    {
        log_i({ "{}: [offset={}|size={}] {:?}" }, __func__, offset, size, path);

        auto res = ok_or(path::create(path), Errc::operation_not_supported).and_then([&](auto p) {
            return get_data().tree().read(p, fi->fh, { buf, size }, offset);
        });
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__, path)(res.error());
    }

    i32 write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi)
    {
        log_i({ "{}: [offset={}|size={}] {:?}" }, __func__, offset, size, path);

        auto res = ok_or(path::create(path), Errc::operation_not_supported).and_then([&](auto p) {
            return get_data().tree().write(p, fi->fh, { buf, size }, offset);
        });
        return res.has_value() ? static_cast<i32>(res.value()) : fuse_err(__func__, path)(res.error());
    }

    i32 flush(const char* path, fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree().flush(p, fi->fh); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }

    i32 release(const char* path, fuse_file_info* fi)
    {
        log_i({ "{}: {:?}" }, __func__, path);

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree().release(p, fi->fh); })
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
    )
    {
        log_i({ "{}: {:?}" }, __func__, path);

        const auto fill = [&](const char* name) { filler(buf, name, nullptr, 0, FUSE_FILL_DIR_PLUS); };

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree().readdir(p, fill); })
            .transform_error(fuse_err(__func__, path))
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

        return ok_or(path::create(path), Errc::operation_not_supported)
            .and_then([&](auto p) { return get_data().tree().utimens(p); })
            .transform_error(fuse_err(__func__, path))
            .error_or(0);
    }
}
