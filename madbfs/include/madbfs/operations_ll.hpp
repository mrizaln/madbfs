#pragma once

#include <madbfs-common/aliases.hpp>

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)
#include <fuse_lowlevel.h>

#include <fcntl.h>

namespace madbfs::ll::operations
{
    void init(void* userdata, fuse_conn_info* conn);

    void destroy(void* userdata);

    void lookup(fuse_req_t req, fuse_ino_t parent, const char* name);

    void forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup);

    void getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);

    void setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, fuse_file_info* fi);

    void readlink(fuse_req_t req, fuse_ino_t ino);

    void mknod(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, dev_t rdev);

    void mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode);

    void unlink(fuse_req_t req, fuse_ino_t parent, const char* name);

    void rmdir(fuse_req_t req, fuse_ino_t parent, const char* name);

    void rename(
        fuse_req_t   req,
        fuse_ino_t   parent,
        const char*  name,
        fuse_ino_t   newparent,
        const char*  newname,
        unsigned int flags
    );

    void open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);

    void read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* fi);

    void write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off, fuse_file_info* fi);

    void flush(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);

    void release(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);

    void fsync(fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info* fi);

    void access(fuse_req_t req, fuse_ino_t ino, int mask);

    void forget_multi(fuse_req_t req, size_t count, fuse_forget_data* forgets);

    void readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* fi);

    void copy_file_range(
        fuse_req_t      req,
        fuse_ino_t      ino_in,
        off_t           off_in,
        fuse_file_info* fi_in,
        fuse_ino_t      ino_out,
        off_t           off_out,
        fuse_file_info* fi_out,
        size_t          len,
        int             flags
    );

    static constexpr auto operations = fuse_lowlevel_ops{
        .init            = madbfs::ll::operations::init,
        .destroy         = madbfs::ll::operations::destroy,
        .lookup          = madbfs::ll::operations::lookup,
        .forget          = madbfs::ll::operations::forget,
        .getattr         = madbfs::ll::operations::getattr,
        .setattr         = madbfs::ll::operations::setattr,
        .readlink        = madbfs::ll::operations::readlink,
        .mknod           = madbfs::ll::operations::mknod,
        .mkdir           = madbfs::ll::operations::mkdir,
        .unlink          = madbfs::ll::operations::unlink,
        .rmdir           = madbfs::ll::operations::rmdir,
        .symlink         = nullptr,
        .rename          = madbfs::ll::operations::rename,
        .link            = nullptr,
        .open            = madbfs::ll::operations::open,
        .read            = madbfs::ll::operations::read,
        .write           = madbfs::ll::operations::write,
        .flush           = madbfs::ll::operations::flush,
        .release         = madbfs::ll::operations::release,
        .fsync           = madbfs::ll::operations::fsync,
        .opendir         = nullptr,
        .readdir         = nullptr,
        .releasedir      = nullptr,
        .fsyncdir        = nullptr,
        .statfs          = nullptr,
        .setxattr        = nullptr,
        .getxattr        = nullptr,
        .listxattr       = nullptr,
        .removexattr     = nullptr,
        .access          = madbfs::ll::operations::access,
        .create          = nullptr,
        .getlk           = nullptr,
        .setlk           = nullptr,
        .bmap            = nullptr,
        .ioctl           = nullptr,
        .poll            = nullptr,
        .write_buf       = nullptr,
        .retrieve_reply  = nullptr,
        .forget_multi    = madbfs::ll::operations::forget_multi,
        .flock           = nullptr,
        .fallocate       = nullptr,
        .readdirplus     = madbfs::ll::operations::readdirplus,
        .copy_file_range = madbfs::ll::operations::copy_file_range,
        .lseek           = nullptr,
        .tmpfile         = nullptr,
        .statx           = nullptr,
    };
}
