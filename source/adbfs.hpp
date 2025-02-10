#pragma once

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fcntl.h>

#include <filesystem>

namespace adbfs
{
    namespace fs = std::filesystem;

    struct AdbfsData
    {
        fs::path    m_dir;
        std::string m_serial;
        bool        m_rescan = false;
    };

    int readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, fuse_file_info* fi);
    int getattr(const char* path, struct stat* stbuf);
    int access(const char* path, int mask);
    int open(const char* path, fuse_file_info* fi);
    int flush(const char* path, fuse_file_info* fi);
    int release(const char* path, fuse_file_info* fi);
    int read(const char* path, char* buf, size_t size, off_t offset, fuse_file_info* fi);
    int write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi);
    int utimens(const char* path, const timespec ts[2]);
    int truncate(const char* path, off_t size);
    int mknod(const char* path, mode_t mode, dev_t rdev);
    int mkdir(const char* path, mode_t mode);
    int rename(const char* from, const char* to);
    int rmdir(const char* path);
    int unlink(const char* path);
    int readlink(const char* path, char* buf, size_t size);
}
