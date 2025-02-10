#include "adbfs.hpp"

namespace adbfs
{
    int readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, fuse_file_info* fi)
    {
        return 0;
    }

    int getattr(const char* path, struct stat* stbuf)
    {
        return 0;
    }

    int access(const char* path, int mask)
    {
        return 0;
    }

    int open(const char* path, fuse_file_info* fi)
    {
        return 0;
    }

    int flush(const char* path, fuse_file_info* fi)
    {
        return 0;
    }

    int release(const char* path, fuse_file_info* fi)
    {
        return 0;
    }

    int read(const char* path, char* buf, size_t size, off_t offset, fuse_file_info* fi)
    {
        return 0;
    }

    int write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi)
    {
        return 0;
    }

    int utimens(const char* path, const timespec ts[2])
    {
        return 0;
    }

    int truncate(const char* path, off_t size)
    {
        return 0;
    }

    int mknod(const char* path, mode_t mode, dev_t rdev)
    {
        return 0;
    }

    int mkdir(const char* path, mode_t mode)
    {
        return 0;
    }

    int rename(const char* from, const char* to)
    {
        return 0;
    }

    int rmdir(const char* path)
    {
        return 0;
    }

    int unlink(const char* path)
    {
        return 0;
    }

    int readlink(const char* path, char* buf, size_t size)
    {
        return 0;
    }
}
