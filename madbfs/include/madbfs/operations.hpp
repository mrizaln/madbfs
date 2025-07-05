#pragma once

#include "madbfs-common/aliases.hpp"

#define FUSE_USE_VERSION 31
#include <fcntl.h>
#include <fuse.h>

namespace madbfs::operations
{
    void* init(fuse_conn_info*, fuse_config*) noexcept;
    void  destroy(void*) noexcept;

    i32 getattr(const char*, struct stat*, fuse_file_info*) noexcept;
    i32 readlink(const char*, char*, usize) noexcept;
    i32 mknod(const char*, mode_t, dev_t) noexcept;
    i32 mkdir(const char*, mode_t) noexcept;
    i32 unlink(const char*) noexcept;
    i32 rmdir(const char*) noexcept;
    i32 rename(const char*, const char*, u32) noexcept;
    i32 truncate(const char*, off_t, fuse_file_info*) noexcept;
    i32 open(const char*, fuse_file_info*) noexcept;
    i32 read(const char*, char*, usize, off_t, fuse_file_info*) noexcept;
    i32 write(const char*, const char*, usize, off_t, fuse_file_info*) noexcept;
    i32 flush(const char*, fuse_file_info*) noexcept;
    i32 release(const char*, fuse_file_info*) noexcept;
    i32 readdir(const char*, void*, fuse_fill_dir_t, off_t, fuse_file_info*, fuse_readdir_flags) noexcept;
    i32 access(const char*, i32) noexcept;
    i32 utimens(const char*, const timespec tv[2], fuse_file_info*) noexcept;

    isize copy_file_range(
        const char*            path_in,
        struct fuse_file_info* fi_in,
        off_t                  offset_in,
        const char*            path_out,
        struct fuse_file_info* fi_out,
        off_t                  offset_out,
        size_t                 size,
        int                    flags
    ) noexcept;

    static constexpr auto operations = fuse_operations{
        .getattr         = madbfs::operations::getattr,
        .readlink        = madbfs::operations::readlink,
        .mknod           = madbfs::operations::mknod,
        .mkdir           = madbfs::operations::mkdir,
        .unlink          = madbfs::operations::unlink,
        .rmdir           = madbfs::operations::rmdir,
        .symlink         = nullptr,
        .rename          = madbfs::operations::rename,
        .link            = nullptr,
        .chmod           = nullptr,
        .chown           = nullptr,
        .truncate        = madbfs::operations::truncate,
        .open            = madbfs::operations::open,
        .read            = madbfs::operations::read,
        .write           = madbfs::operations::write,
        .statfs          = nullptr,
        .flush           = madbfs::operations::flush,
        .release         = madbfs::operations::release,
        .fsync           = nullptr,
        .setxattr        = nullptr,
        .getxattr        = nullptr,
        .listxattr       = nullptr,
        .removexattr     = nullptr,
        .opendir         = nullptr,
        .readdir         = madbfs::operations::readdir,
        .releasedir      = nullptr,
        .fsyncdir        = nullptr,
        .init            = madbfs::operations::init,       // entry point of fuse_main
        .destroy         = madbfs::operations::destroy,    // exit point of fuse_main
        .access          = madbfs::operations::access,
        .create          = nullptr,
        .lock            = nullptr,
        .utimens         = madbfs::operations::utimens,
        .bmap            = nullptr,
        .ioctl           = nullptr,
        .poll            = nullptr,
        .write_buf       = nullptr,
        .read_buf        = nullptr,
        .flock           = nullptr,
        .fallocate       = nullptr,
        .copy_file_range = madbfs::operations::copy_file_range,
        .lseek           = nullptr,
    };
}
