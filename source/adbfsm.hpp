#pragma once

#include "common.hpp"
#include "log.hpp"

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fcntl.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace adbfsm
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

    class LocalCopy
    {
    public:
        LocalCopy(usize max_size)
            : m_max_size(max_size)
        {
        }

        // return false if file is too big
        bool add(fs::path path)
        {
            auto lock = std::unique_lock{ m_mutex };

            if (fs::file_size(path) > m_max_size) {
                return false;
            }

            auto new_size = m_current_size + fs::file_size(path);
            while (new_size > m_max_size) {
                log_d({ "local copy: too big ({} > {}), removing oldest file" }, new_size, m_max_size);
                new_size -= fs::file_size(m_files.front());
                fs::remove(m_files.front());
                m_files_set.erase(m_files.front());
                m_files.pop_front();
            }

            log_d({ "local copy: adding file {:?} (size={}|max={})" }, path, new_size, m_max_size);

            m_files.push_back(std::move(path));
            m_files_set.insert(m_files.back());
            m_current_size = new_size;

            return true;
        }

        bool exists(const fs::path& path)
        {
            auto lock = std::shared_lock{ m_mutex };
            return m_files_set.contains(path);
        }

        void remove(const fs::path& path)
        {
            auto lock = std::unique_lock{ m_mutex };

            auto erased = std::erase_if(m_files, [&](auto&& p) { return p == path; });
            if (erased) {
                log_d({ "local copy: removing file {:?}" }, path);
                m_current_size -= fs::file_size(path);
                m_files_set.erase(path);
                fs::remove(path);
            }
        }

        void rename(const fs::path& from, fs::path&& to)
        {
            auto lock = std::unique_lock{ m_mutex };

            auto found = sr::find(m_files.begin(), m_files.end(), from);
            if (found != m_files.end()) {
                m_files_set.erase(*found);
                *found = std::move(to);
                m_files_set.insert(*found);
            }
        }

        usize max_size() const { return m_max_size; }

    private:
        std::deque<fs::path>         m_files;
        std::unordered_set<fs::path> m_files_set;
        mutable std::shared_mutex    m_mutex;

        usize m_current_size = 0;
        usize m_max_size     = 0;
    };

    struct Cache
    {
        std::unordered_map<std::string, Stat, StrHash, std::equal_to<>> m_file_stat;
        std::unordered_set<std::string, StrHash, std::equal_to<>>       m_file_truncated;
        std::unordered_set<int>                                         m_file_pending_write;

        std::unordered_map<std::string, int, StrHash, std::equal_to<>> m_uid;
        std::unordered_map<std::string, int, StrHash, std::equal_to<>> m_gid;

        std::shared_mutex m_mutex;
    };

    struct AdbfsmData
    {
        Cache             m_cache;
        LocalCopy         m_local_copy;
        fs::path          m_dir;
        std::string       m_serial;
        std::atomic<bool> m_readdir = false;
        bool              m_rescan  = false;
    };

    void destroy(void* private_data);

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
