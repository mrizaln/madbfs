#pragma once

#include "madbfs/connection/connection.hpp"
#include "madbfs/data/ipc.hpp"
#include "madbfs/tree/file_tree.hpp"

#define FUSE_USE_VERSION 31
#include <fcntl.h>
#include <fuse3/fuse.h>

#include <thread>

namespace madbfs
{
    /**
     * @class Madbfs
     *
     * @brief Main class of the filesystem.
     *
     * Everything is controlled from here. An instantiation of this class will live as long as the fs mounted.
     * The instance will be available as fuse_context's private data.
     */
    class Madbfs
    {
    public:
        Madbfs(usize page_size, usize max_pages);
        ~Madbfs();

        tree::FileTree&    tree() { return m_tree; }
        async::Context&    async_ctx() { return m_async_ctx; }
        const data::Cache& cache() const { return m_cache; }

    private:
        boost::json::value ipc_handler(data::ipc::Op op);

        async::Context   m_async_ctx;
        async::WorkGuard m_work_guard;    // to prevent `io_context` from returning immediately
        std::jthread     m_work_thread;

        Uniq<connection::Connection> m_connection;
        data::Cache                  m_cache;
        tree::FileTree               m_tree;
        Uniq<data::Ipc>              m_ipc;
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

    isize copy_file_range(
        const char*            path_in,
        struct fuse_file_info* fi_in,
        off_t                  offset_in,
        const char*            path_out,
        struct fuse_file_info* fi_out,
        off_t                  offset_out,
        size_t                 size,
        int                    flags
    );

    static constexpr auto operations = fuse_operations{
        .getattr         = madbfs::getattr,
        .readlink        = madbfs::readlink,
        .mknod           = madbfs::mknod,
        .mkdir           = madbfs::mkdir,
        .unlink          = madbfs::unlink,
        .rmdir           = madbfs::rmdir,
        .symlink         = nullptr,
        .rename          = madbfs::rename,
        .link            = nullptr,
        .chmod           = nullptr,
        .chown           = nullptr,
        .truncate        = madbfs::truncate,
        .open            = madbfs::open,
        .read            = madbfs::read,
        .write           = madbfs::write,
        .statfs          = nullptr,
        .flush           = madbfs::flush,
        .release         = madbfs::release,
        .fsync           = nullptr,
        .setxattr        = nullptr,
        .getxattr        = nullptr,
        .listxattr       = nullptr,
        .removexattr     = nullptr,
        .opendir         = nullptr,
        .readdir         = madbfs::readdir,
        .releasedir      = nullptr,
        .fsyncdir        = nullptr,
        .init            = madbfs::init,
        .destroy         = madbfs::destroy,
        .access          = madbfs::access,
        .create          = nullptr,
        .lock            = nullptr,
        .utimens         = madbfs::utimens,
        .bmap            = nullptr,
        .ioctl           = nullptr,
        .poll            = nullptr,
        .write_buf       = nullptr,
        .read_buf        = nullptr,
        .flock           = nullptr,
        .fallocate       = nullptr,
        .copy_file_range = madbfs::copy_file_range,
        .lseek           = nullptr,
    };
}
