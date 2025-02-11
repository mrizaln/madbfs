#pragma once

#include "common.hpp"
#include "log.hpp"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
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

        usize operator()(Str txt) const { return std::hash<Str>{}(txt); }
        usize operator()(const char* txt) const { return std::hash<Str>{}(txt); }
        usize operator()(const String& txt) const { return std::hash<String>{}(txt); }
    };

    struct Stat
    {
        i32       m_mode  = 0;    // -rwxrwxrwx
        i32       m_links = 1;
        i32       m_uid   = 0;
        i32       m_gid   = 0;
        i32       m_size  = 0;
        Timestamp m_mtime = {};    // last modification time
        Timestamp m_age   = {};    // cache age

        String m_link_to = {};    // only makes sense if (m_mode & S_IFLNK) is non-zero
        i32    m_err     = 0;
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

    template <typename Value>
    using StringMap = std::unordered_map<String, Value, StrHash, std::equal_to<>>;
    using StringSet = std::unordered_set<String, StrHash, std::equal_to<>>;

    struct Cache
    {
        StringMap<Stat>         m_file_stat;
        StringSet               m_file_truncated;
        std::unordered_set<i32> m_file_pending_write;

        StringMap<i32> m_uid;
        StringMap<i32> m_gid;

        std::shared_mutex m_mutex;
    };

    struct AdbfsmData
    {
        Cache             m_cache;
        LocalCopy         m_local_copy;
        fs::path          m_dir;
        String            m_serial;
        std::atomic<bool> m_readdir = false;
        bool              m_rescan  = false;
    };

    void destroy(void* private_data);

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

    constexpr auto operations = fuse_operations{
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
        .init            = nullptr,
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
