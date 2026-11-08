#include "madbfs/operations_ll.hpp"

#include "madbfs/bridge.hpp"

#include <madbfs-common/log.hpp>

using namespace madbfs;

static constexpr auto required_capabilities = std::to_array<u32>({
    FUSE_CAP_READDIRPLUS,
    FUSE_CAP_CACHE_SYMLINKS,
    FUSE_CAP_NO_OPENDIR_SUPPORT,
});

static constexpr auto unsupported_capabilities = std::to_array<u32>({
    FUSE_CAP_ATOMIC_O_TRUNC,
});

namespace
{
    ll::Bridge& get_bridge(fuse_req_t req)
    {
        auto ptr = fuse_req_userdata(req);
        return *static_cast<ll::Bridge*>(ptr);
    }

    void logged_reply_err(
        const char*          name,
        fuse_req_t           req,
        std::errc            err,
        std::source_location loc = std::source_location::current()
    )
    {
        using log::Level;
        auto log = [=]<typename... Args>(Level level, fmt::format_string<Args...>&& fmt, Args&&... args) {
            log::log_loc_named(loc, level, name, std::move(fmt), std::forward<Args>(args)...);
        };

        const auto errint = static_cast<int>(err);
        if (errint == 0) {
            fuse_reply_err(req, errint);
            return;
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
                log(Level::warn, "returned with error code [{}]: {}", errint, msg);
            }
        } break;
        default: {
            const auto& msg = err_msg(err);
            log(Level::err, "returned with error code [{}]: {}", errint, msg);
        }
        }

        fuse_reply_err(req, errint);
    }

    Expect<void> resolve_symlink(Span<char> buf, Str target, Str mount_point)
    {
        if (target.size() < 1) {
            return Unexpect{ Errc::invalid_argument };
        }

        // custom root may break symlinks since the path pointed by the link might be unreachable, do I need
        // to handle this special case? currently I'm too lazy to think, so I'll just let it be.

        // FIXME: this transformation mess with the size reported on getattr/lookup

        if (target[0] == '/') {    // absolute link
            auto mount = mount_point;
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

// operations_ll.hpp impl
namespace madbfs::ll::operations
{
    void init(void* userdata, fuse_conn_info* conn)
    {
        /// see `madbfs::ll::run()`
        auto& bridge = *static_cast<Bridge*>(userdata);

        for (auto cap : required_capabilities) {
            if (auto success = fuse_set_feature_flag(conn, cap); not success) {
                log_w(__func__, "failed to set capabilty: {}", cap);
            }
        }

        if (bridge.writeback()) {
            if (auto success = fuse_set_feature_flag(conn, FUSE_CAP_WRITEBACK_CACHE); not success) {
                bridge.set_writeback(false);
                log_w(__func__, "writeback cache is not supported");
            }
        }

        for (auto cap : unsupported_capabilities) {
            fuse_unset_feature_flag(conn, cap);
        }
    }

    void destroy(void* /* userdata */)
    {
        // do nothing
    }

    void lookup(fuse_req_t req, fuse_ino_t parent, const char* name)
    {
        log_i(__func__, "parent={:016x}, name={}", parent, name);

        auto& bridge    = get_bridge(req);
        auto  parent_id = Id::from_u64(parent);

        struct stat stat = {};

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_lookup, parent_id, name, &stat); ret) {
            auto entry = fuse_entry_param{
                .ino           = stat.st_ino,
                .generation    = 0,
                .attr          = stat,
                .attr_timeout  = static_cast<double>(bridge.ttl().count()),
                .entry_timeout = static_cast<double>(bridge.ttl().count()),
            };
            fuse_reply_entry(req, &entry);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
    {
        log_i(__func__, "ino={:016x}, nlookup={}", ino, nlookup);

        auto& bridge = get_bridge(req);
        auto  id     = Id::from_u64(ino);

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_forget, id, nlookup); not ret) {
            log_w(__func__, "failure when forget: {}", err_msg(ret.error()));
        }

        fuse_reply_none(req);
    }

    void getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* /* fi */)
    {
        log_i(__func__, "ino={:016x}", ino);

        auto& bridge = get_bridge(req);
        auto  id     = Id::from_u64(ino);

        struct stat stat = {};

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_getattr, id, &stat); ret) {
            auto timeout = static_cast<double>(bridge.ttl().count());
            fuse_reply_attr(req, &stat, timeout);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, fuse_file_info* /* fi */)
    {
        log_i(__func__, "ino={:016x}, to_set={}", ino, to_set);

        if (to_set & FUSE_SET_ATTR_MODE) {
            logged_reply_err(__func__, req, std::errc::function_not_supported);
            return;
        } else if (to_set & FUSE_SET_ATTR_UID) {
            logged_reply_err(__func__, req, std::errc::function_not_supported);
            return;
        } else if (to_set & FUSE_SET_ATTR_GID) {
            logged_reply_err(__func__, req, std::errc::function_not_supported);
            return;
        }

        auto& bridge = get_bridge(req);
        auto  id     = Id::from_u64(ino);
        auto  size   = to_set & FUSE_SET_ATTR_SIZE ? Opt<u64>{ attr->st_size } : std::nullopt;
        auto  atime  = to_set & FUSE_SET_ATTR_ATIME ? Opt<timespec>{ attr->st_atim } : std::nullopt;
        auto  mtime  = to_set & FUSE_SET_ATTR_MTIME ? Opt<timespec>{ attr->st_mtim } : std::nullopt;

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_setattr, id, size, atime, mtime, attr); ret) {
            auto timeout = static_cast<double>(bridge.ttl().count());
            fuse_reply_attr(req, attr, timeout);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void readlink(fuse_req_t req, fuse_ino_t ino)
    {
        log_i(__func__, "ino={:016x}", ino);

        auto& bridge = get_bridge(req);
        auto  id     = Id::from_u64(ino);
        auto  buf    = std::array<char, PATH_MAX>{};

        auto ret = bridge.invoke_fs(&Filesystem::ll_readlink, id).and_then([&](Str link) {
            log_d("readlink", "link target: {}", link);
            return resolve_symlink(buf, link, bridge.mount());
        });

        if (ret) {
            fuse_reply_readlink(req, buf.data());
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void mknod(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, dev_t rdev)
    {
        log_i(__func__, "parent={:016x}, name={}, mode={:0o}, dev={}", parent, name, mode, rdev);

        auto& bridge    = get_bridge(req);
        auto  parent_id = Id::from_u64(parent);

        struct stat stat = {};

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_mknod, parent_id, name, mode, rdev, &stat); ret) {
            auto entry = fuse_entry_param{
                .ino           = stat.st_ino,
                .generation    = 0,
                .attr          = stat,
                .attr_timeout  = static_cast<double>(bridge.ttl().count()),
                .entry_timeout = static_cast<double>(bridge.ttl().count()),
            };
            fuse_reply_entry(req, &entry);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode)
    {
        log_i(__func__, "parent={:016x}, name={}, mode={:0o}", parent, name, mode);

        auto& bridge    = get_bridge(req);
        auto  parent_id = Id::from_u64(parent);

        struct stat stat = {};

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_mkdir, parent_id, name, mode, &stat); ret) {
            auto entry = fuse_entry_param{
                .ino           = stat.st_ino,
                .generation    = 0,
                .attr          = stat,
                .attr_timeout  = static_cast<double>(bridge.ttl().count()),
                .entry_timeout = static_cast<double>(bridge.ttl().count()),
            };
            fuse_reply_entry(req, &entry);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void unlink(fuse_req_t req, fuse_ino_t parent, const char* name)
    {
        log_i(__func__, "parent={:016x}, name={}", parent, name);

        auto& bridge    = get_bridge(req);
        auto  parent_id = Id::from_u64(parent);

        auto ret = bridge.invoke_fs(&Filesystem::ll_unlink, parent_id, name);
        logged_reply_err(__func__, req, ret.error_or({}));
    }

    void rmdir(fuse_req_t req, fuse_ino_t parent, const char* name)
    {
        log_i(__func__, "parent={:016x}, name={}", parent, name);

        auto& bridge    = get_bridge(req);
        auto  parent_id = Id::from_u64(parent);

        auto ret = bridge.invoke_fs(&Filesystem::ll_rmdir, parent_id, name);
        logged_reply_err(__func__, req, ret.error_or({}));
    }

    void rename(
        fuse_req_t   req,
        fuse_ino_t   parent,
        const char*  name,
        fuse_ino_t   newparent,
        const char*  newname,
        unsigned int flags
    )
    {
        log_i(
            __func__,
            "parent={:016x}, name={}, newparent={:016x}, newname={}, flags={:0x}",
            parent,
            name,
            newparent,
            newname,
            flags
        );

        auto& bridge       = get_bridge(req);
        auto  parent_id    = Id::from_u64(parent);
        auto  newparent_id = Id::from_u64(newparent);

        auto ret = bridge.invoke_fs(&Filesystem::ll_rename, parent_id, name, newparent_id, newname, flags);
        logged_reply_err(__func__, req, ret.error_or({}));
    }

    void open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
    {
        log_i(__func__, "ino={:016x}, flags={:0x}", ino, fi->flags);

        auto& bridge = get_bridge(req);
        auto  id     = Id::from_u64(ino);
        auto  flags  = fi->flags;

        if (bridge.writeback() and (flags & O_WRONLY) != 0) {
            // on writeback mode, kernel may send read request as well
            flags = (flags & ~O_ACCMODE) | O_RDWR;
        }

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_open, id, flags); ret) {
            fi->fh = ret.value();
            fuse_reply_open(req, fi);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* fi)
    {
        log_i(__func__, "ino={:016x}, size={}, off={}, fh={}", ino, size, off, fi->fh);

        auto& bridge = get_bridge(req);
        auto  out    = Vec<char>(size);    // TODO: buffer pooling

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_read, fi->fh, Span{ out }, off); ret) {
            fuse_reply_buf(req, out.data(), ret.value());
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off, fuse_file_info* fi)
    {
        log_i(__func__, "ino={:016x}, size={}, off={}, fh={}", ino, size, off, fi->fh);

        auto& bridge = get_bridge(req);
        auto  in     = Span{ buf, size };

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_write, fi->fh, in, off); ret) {
            fuse_reply_write(req, ret.value());
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void flush(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
    {
        log_i(__func__, "ino={:016x}, fh={}", ino, fi->fh);

        auto& bridge = get_bridge(req);

        auto ret = bridge.invoke_fs(&Filesystem::ll_fsync, fi->fh, true);
        logged_reply_err(__func__, req, ret.error_or({}));
    }

    void release(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
    {
        log_i(__func__, "ino={:016x}, fh={}", ino, fi->fh);

        auto& bridge = get_bridge(req);

        auto ret = bridge.invoke_fs(&Filesystem::ll_release, fi->fh);
        logged_reply_err(__func__, req, ret.error_or({}));
    }

    void fsync(fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info* fi)
    {
        log_i(__func__, "ino={:016x}, datasync={}, fh={}", ino, datasync, fi->fh);

        auto& bridge = get_bridge(req);

        auto ret = bridge.invoke_fs(&Filesystem::ll_fsync, fi->fh, datasync);
        logged_reply_err(__func__, req, ret.error_or({}));
    }

    void access(fuse_req_t req, fuse_ino_t ino, int mask)
    {
        log_i(__func__, "ino={:016x}, mask={}", ino, mask);
        fuse_reply_err(req, 0);    // ok
    }

    void forget_multi(fuse_req_t req, size_t count, fuse_forget_data* forgets)
    {
        log_i(__func__, "count={}", count);

        auto& bridge = get_bridge(req);

        for (auto i : sv::iota(0uz, count)) {
            auto data = forgets[i];
            auto id   = Id::from_u64(data.ino);
            if (auto ret = bridge.invoke_fs(&Filesystem::ll_forget, id, data.nlookup); not ret) {
                log_w(__func__, "failure when forget: {}", err_msg(ret.error()));
            }
        }

        fuse_reply_none(req);
    }

    void readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* /* fi */)
    {
        log_i(__func__, "ino={:016x}, size={}, off={}", ino, size, off);

        auto& bridge = get_bridge(req);
        auto  id     = Id::from_u64(ino);
        auto  buf    = Vec<char>(size);    // TODO: buffer pooling
        auto  cur    = 0uz;

        const auto filler = [&](const char* name, const struct stat* stbuf, off_t offset) {
            auto rel_buf  = buf.data() + cur;
            auto rel_size = buf.size() - cur;

            auto entry = fuse_entry_param{
                .ino           = stbuf->st_ino,
                .generation    = 0,
                .attr          = *stbuf,
                .attr_timeout  = static_cast<double>(bridge.ttl().count()),
                .entry_timeout = static_cast<double>(bridge.ttl().count()),
            };

            auto count = fuse_add_direntry_plus(req, rel_buf, rel_size, name, &entry, offset);
            if (count <= rel_size) {
                cur += count;
                return false;
            } else {
                return true;    // filler is full when the size of entry is bigger than the remaining buffer
            }
        };

        if (auto ret = bridge.invoke_fs(&Filesystem::ll_readdir, id, off, filler); ret) {
            fuse_reply_buf(req, buf.data(), cur);
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }

    void copy_file_range(
        fuse_req_t      req,
        fuse_ino_t      ino_in,
        off_t           off_in,
        fuse_file_info* fi_in,
        fuse_ino_t      ino_out,
        off_t           off_out,
        fuse_file_info* fi_out,
        size_t          len,
        int /* flags */
    )
    {
        log_i(
            __func__,
            "ino_in={:016x}, off_in={}, fh_in={}, ino_out={:016x}, off_out={}, fh_out={}, len={}",
            ino_in,
            off_in,
            fi_in->fh,
            ino_out,
            off_out,
            fi_out->fh,
            len
        );

        auto& bridge = get_bridge(req);
        auto  id_in  = Id::from_u64(ino_in);
        auto  id_out = Id::from_u64(ino_out);

        auto op  = &Filesystem::ll_copy_file_range;
        auto ret = bridge.invoke_fs(op, id_in, fi_in->fh, off_in, id_out, fi_out->fh, off_out, len);

        if (ret) {
            fuse_reply_write(req, ret.value());
        } else {
            logged_reply_err(__func__, req, ret.error());
        }
    }
}
