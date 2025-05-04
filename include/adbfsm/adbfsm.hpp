#pragma once

#include "adbfsm/data/connection.hpp"
#include "adbfsm/tree/file_tree.hpp"

#define FUSE_USE_VERSION 31
#include <fcntl.h>
#include <fuse3/fuse.h>

namespace adbfsm
{
    struct Adbfsm
    {
        Uniq<data::IConnection> connection;
        Uniq<data::Cache>       cache;
        tree::FileTree          tree;
    };

    void* init(fuse_conn_info*, fuse_config*);
    void  destroy(void*);

    i32 getattr(const char*, struct stat*, fuse_file_info*);
    i32 readlink(const char*, char*, usize);
    i32 mknod(const char*, mode_t, dev_t);
    i32 mkdir(const char*, mode_t);
    i32 unlink(const char*);
    i32 rmdir(const char*);
    i32 rename(const char*, const char*, u32);
    i32 truncate(const char*, off_t, fuse_file_info*);
    i32 open(const char*, fuse_file_info*);
    i32 read(const char*, char*, usize, off_t, fuse_file_info*);
    i32 write(const char*, const char*, usize, off_t, fuse_file_info*);
    i32 flush(const char*, fuse_file_info*);
    i32 release(const char*, fuse_file_info*);
    i32 readdir(const char*, void*, fuse_fill_dir_t, off_t, fuse_file_info*, fuse_readdir_flags);
    i32 access(const char*, i32);
    i32 utimens(const char*, const timespec tv[2], fuse_file_info*);

    static constexpr auto operations = fuse_operations{
        .getattr         = adbfsm::getattr,
        .readlink        = adbfsm::readlink,
        .mknod           = adbfsm::mknod,
        .mkdir           = adbfsm::mkdir,
        .unlink          = adbfsm::unlink,
        .rmdir           = adbfsm::rmdir,
        .symlink         = nullptr,
        .rename          = adbfsm::rename,
        .link            = nullptr,
        .chmod           = nullptr,
        .chown           = nullptr,
        .truncate        = adbfsm::truncate,
        .open            = adbfsm::open,
        .read            = adbfsm::read,
        .write           = adbfsm::write,
        .statfs          = nullptr,
        .flush           = adbfsm::flush,
        .release         = adbfsm::release,
        .fsync           = nullptr,
        .setxattr        = nullptr,
        .getxattr        = nullptr,
        .listxattr       = nullptr,
        .removexattr     = nullptr,
        .opendir         = nullptr,
        .readdir         = adbfsm::readdir,
        .releasedir      = nullptr,
        .fsyncdir        = nullptr,
        .init            = adbfsm::init,
        .destroy         = adbfsm::destroy,
        .access          = adbfsm::access,
        .create          = nullptr,
        .lock            = nullptr,
        .utimens         = adbfsm::utimens,
        .bmap            = nullptr,
        .ioctl           = nullptr,
        .poll            = nullptr,
        .write_buf       = nullptr,
        .read_buf        = nullptr,
        .flock           = nullptr,
        .fallocate       = nullptr,
        .copy_file_range = nullptr,
        .lseek           = nullptr,
    };
}
