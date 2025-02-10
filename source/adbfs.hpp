#pragma once

#include "common.hpp"
#include <atomic>

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fcntl.h>

#include <chrono>
#include <shared_mutex>
#include <unordered_map>
#include <filesystem>

namespace adbfs
{
    namespace fs = std::filesystem;

    using Clock     = std::chrono::system_clock;
    using Timestamp = Clock::time_point;

    struct StrHash
    {
        using is_transparent = void;

        size_t operator()(Str txt) const { return std::hash<Str>{}(txt); }
        size_t operator()(const char* txt) const { return std::hash<Str>{}(txt); }
        size_t operator()(const std::string& txt) const { return std::hash<std::string>{}(txt); }
    };

    struct Stat
    {
        int       m_mode  = 0;    // -rwxrwxrwx
        int       m_links = 1;
        int       m_uid   = 0;
        int       m_gid   = 0;
        int       m_size  = 0;
        Timestamp m_mtime = {};    // last modification time
        Timestamp m_age   = {};    // cache age

        std::string m_link_to = {};    // only makes sense if (m_mode & S_IFLNK) is non-zero
        int         m_err     = 0;
    };

    struct Cache
    {
        std::unordered_map<std::string, Stat, StrHash, std::equal_to<>> m_file_stat;
        std::unordered_map<std::string, int, StrHash, std::equal_to<>>  m_uid;
        std::unordered_map<std::string, int, StrHash, std::equal_to<>>  m_gid;

        std::shared_mutex m_mutex;
    };

    struct AdbfsData
    {
        Cache             m_cache;
        fs::path          m_dir;
        std::string       m_serial;
        std::atomic<bool> m_readdir = false;
        bool              m_rescan  = false;
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
