#include "cmd.hpp"
#include "common.hpp"
#include "util.hpp"
#include "adbfs.hpp"

#include <fuse.h>

#include <charconv>
#include <cassert>

namespace detail
{
    using namespace adbfs;

    static constexpr usize minimum_path_size   = 3;
    static constexpr int   default_gid         = 98;
    static constexpr int   default_uid         = 98;
    static constexpr Str   no_device           = "adb: no devices/emulators found";
    static constexpr Str   permission_denied   = "Permission denied";
    static constexpr Str   no_such_file_or_dir = "No such file or directory";
    static constexpr Str   not_a_directory     = "Not a directory";
    static constexpr Str   inaccessible        = "inaccessible or not found";

    struct ParsedStat
    {
        Str   m_path;
        Stat& m_stat;
    };

    enum class Error
    {
        Unknown,
        NoDev,
        PermDenied,
        NoSuchFileOrDir,
        NotADir,
        Inaccessible
    };

    int to_errno(Error err)
    {
        switch (err) {
        case Error::Unknown: return EIO;
        case Error::NoDev: return ENODEV;
        case Error::PermDenied: return EACCES;
        case Error::NoSuchFileOrDir: return ENOENT;
        case Error::NotADir: return ENOTDIR;
        case Error::Inaccessible: return ENOMEDIUM;    // program not accessible means no medium, right?
        }
    }

    Str to_string(Error err)
    {
        switch (err) {
        case Error::Unknown: return "Unknown error";
        case Error::NoDev: return "No device";
        case Error::PermDenied: return "Permission denied";
        case Error::NoSuchFileOrDir: return "No such file or directory";
        case Error::NotADir: return "Not a directory";
        case Error::Inaccessible: return "Inaccessible or not found";
        }
    }

    AdbfsData& get_data()
    {
        auto ctx = fuse_get_context()->private_data;
        assert(ctx != nullptr);
        return *static_cast<AdbfsData*>(ctx);
    }

    std::string copy_replace(const char* str, char replace, char with)
    {
        auto  new_str = std::string{};
        auto* ch      = str;
        while (ch != nullptr) {
            *ch == replace ? new_str.push_back(with) : new_str.push_back(*ch);
            ++ch;
        }
        return new_str;
    }

    template <typename T>
        requires std::is_fundamental_v<T>
    constexpr Opt<T> parse_fundamental(Str str)
    {
        auto t         = T{};
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), t);
        if (ptr != str.data() + str.size() or ec != std::errc{}) {
            return {};
        }
        return t;
    }

    Error parse_stderr(Str str)
    {
        auto splitter = util::StringSplitter{ str, { '\n' } };
        while (auto line = splitter.next()) {
            if (*line == no_device) {
                return Error::NoDev;
            }

            auto rev = std::string{ line->rbegin(), line->rend() };
            auto err = util::StringSplitter{ rev, { ':' } }.next();
            if (not err) {
                continue;
            }

            auto eq = [](auto lhs, auto rhs) { return sr::equal(lhs, rhs | sv::reverse); };

            if (eq(*err, permission_denied)) {
                return Error::PermDenied;
            } else if (eq(*err, no_such_file_or_dir)) {
                return Error::NoSuchFileOrDir;
            } else if (eq(*err, not_a_directory)) {
                return Error::NotADir;
            }
        }

        return Error::Unknown;
    }

    int parse_mode(Str str)
    {
        if (str.size() != 10) {
            return 0;
        }

        auto fmode = 0;

        switch (str[0]) {
        case 's': fmode |= S_IFSOCK; break;
        case 'l': fmode |= S_IFLNK; break;
        case '-': fmode |= S_IFREG; break;
        case 'd': fmode |= S_IFDIR; break;
        case 'b': fmode |= S_IFBLK; break;
        case 'c': fmode |= S_IFCHR; break;
        case 'p': fmode |= S_IFIFO; break;
        }

        fmode |= str[1] == 'r' ? S_IRUSR : 0;
        fmode |= str[2] == 'w' ? S_IWUSR : 0;

        switch (str[3]) {
        case 'x': fmode |= S_IXUSR; break;
        case 's': fmode |= S_ISUID | S_IXUSR; break;
        case 'S': fmode |= S_ISUID; break;
        }

        fmode |= str[4] == 'r' ? S_IRGRP : 0;
        fmode |= str[5] == 'w' ? S_IWGRP : 0;

        switch (str[6]) {
        case 'x': fmode |= S_IXGRP; break;
        case 's': fmode |= S_ISGID | S_IXGRP; break;
        case 'S': fmode |= S_ISGID; break;
        }

        fmode |= str[7] == 'r' ? S_IROTH : 0;
        fmode |= str[8] == 'w' ? S_IWOTH : 0;

        switch (str[9]) {
        case 'x': fmode |= S_IXOTH; break;
        case 't': fmode |= S_ISVTX | S_IXOTH; break;
        case 'T': fmode |= S_ISVTX; break;
        }

        return fmode;
    }

    int get_gid(Str group, Cache& cache, Str serial)
    {
        {
            auto lock = std::shared_lock{ cache.m_mutex };
            if (auto it = cache.m_gid.find(group); it != cache.m_gid.end()) {
                return it->second;
            }
        }

        auto gid = cmd::exec_adb({ "id", "-g", std::string{ group } }, serial);
        if (gid.returncode != 0) {
            log_e({ "get_gid: failed to get gid for [{}] (err: {}) {}" }, group, gid.returncode, gid.cerr);
        }

        auto lock     = std::unique_lock{ cache.m_mutex };
        auto real_gid = parse_fundamental<int>(gid.cout).value_or(default_gid);

        return cache.m_gid[std::string{ group }] = real_gid;
    }

    int get_uid(Str user, Cache& cache, Str serial)
    {
        {
            auto lock = std::shared_lock{ cache.m_mutex };
            if (auto it = cache.m_uid.find(user); it != cache.m_uid.end()) {
                return it->second;
            }
        }

        auto uid = cmd::exec_adb({ "id", "-u", std::string{ user } }, serial);
        if (uid.returncode != 0) {
            log_e({ "get_uid: failed to get uid for [{}] (err: {}) {}" }, user, uid.returncode, uid.cerr);
        }

        auto lock     = std::unique_lock{ cache.m_mutex };
        auto real_uid = parse_fundamental<int>(uid.cout).value_or(default_uid);

        return cache.m_uid[std::string{ user }] = real_uid;
    }

    Opt<Timestamp> parse_time(Str ymd, Str hmsms, Str offset)
    {
        // using std::chrono::parse function (the milliseconds part is not able to be parsed)
        // parseable format: "2024-11-02 20:09:18 -0700"
        auto format = "%F %T %z";
        auto hms    = hmsms.substr(0, hmsms.find_first_of('.'));
        auto buffer = std::stringstream{};

        // clang-format off
        for (auto ch : ymd)    { buffer << ch; } buffer << ' ';
        for (auto ch : hms)    { buffer << ch; } buffer << ' ';
        for (auto ch : offset) { buffer << ch; }
        // clang-format on

        auto time = Timestamp{};
        if (buffer >> std::chrono::parse(format, time)) {
            return time;
        };

        log_e({ "parse_time: failed to parse time [{} {} {}]" }, ymd, hms, offset);
        return {};
    }

    /**
     * @brief Parse the output of `ls -lla` command.
     *
     * @param str The output of `ls -lla` command.
     * @param cache The cache to store the parsed stat.
     * @param serial The serial number of the device.
     *
     * @return The parsed stat if successful, otherwise empty.
     *
     * example input:
     *  -rw-rw----  1 root everybody 16037494 2025-02-02 03:50:34.000000000 +0700 pozy.qoi
     *
     * block device and character device are not supported currently:
     *  crw-rw-rw-  1 root root        1,   5 2025-02-09 21:09:39.728000000 +0700 zero
     *
     * file with unknown stat field will not be parsed:
     *  d?????????   ? ?      ?             ?                             ? metadata
     */
    template <std::invocable<ParsedStat> Fn>
    bool parse_file_stat(Str str, Cache& cache, Str serial, Fn&& fn)
    {
        auto result = util::split_n<8>(str, { ' ' });
        if (not result) {
            return false;
        }

        auto [to_be_stat, remainder] = *result;

        auto path = remainder;
        auto link = std::string_view{};

        if (auto arrow = remainder.find(" -> "); arrow != std::string::npos) {
            path = remainder.substr(0, arrow);
            link = remainder.substr(arrow + 4);
        }

        if (to_be_stat[0].find_first_of('?') != std::string::npos) {
            log_e({ "parse_file_stat: failed, file contains unparsable data [{}]" }, to_be_stat[0]);
            return false;
        }

        {
            auto lock = std::shared_lock{ cache.m_mutex };
            if (auto found = cache.m_file_stat.find(path); found != cache.m_file_stat.end()) {
                auto& [path, stat] = *found;
                fn(ParsedStat{ .m_path = path, .m_stat = stat });
                return true;
            }
        }

        auto stat = Stat{
            .m_mode    = parse_mode(to_be_stat[0]),
            .m_links   = parse_fundamental<int>(to_be_stat[1]).value(),
            .m_uid     = get_uid(to_be_stat[2], cache, serial),
            .m_gid     = get_gid(to_be_stat[3], cache, serial),
            .m_size    = parse_fundamental<int>(to_be_stat[4]).value(),
            .m_mtime   = parse_time(to_be_stat[5], to_be_stat[6], to_be_stat[7]).value(),
            .m_age     = Clock::now(),
            .m_link_to = std::string{ link },
        };

        if ((stat.m_mode & S_IFMT) == S_IFLNK and link.empty()) {
            log_c({ "parse_file_stat: link is empty for [{}] when it should not be" }, path);
        }

        auto it = cache.m_file_stat.begin();
        {
            auto lock = std::unique_lock{ cache.m_mutex };
            it        = cache.m_file_stat.emplace(path, stat).first;
        }

        fn(ParsedStat{ .m_path = it->first, .m_stat = it->second });
        return true;
    }
}

namespace adbfs
{
    int readdir(
        const char*                      path,
        void*                            buf,
        fuse_fill_dir_t                  filler,
        [[maybe_unused]] off_t           offset,
        [[maybe_unused]] fuse_file_info* fi
    )
    {
        log_i({ "readdir: {}" }, path);

        auto& [cache, temp, serial, _] = detail::get_data();

        if (auto found = cache.m_file_stat.find(path); found != cache.m_file_stat.end()) {
            auto& [path, stat] = *found;
            if (stat.m_err != 0) {
                return -stat.m_err;
            }
        }

        auto ls = cmd::exec_adb({ "ls", "-lla", path }, serial);
        if (ls.returncode != 0) {
            auto errn = to_errno(detail::parse_stderr(ls.cerr));
            log_e({ "readdir: failed to list directory [{}] (err: {}) {}" }, ls.returncode, errn, ls.cerr);
            if (errn != ENODEV) {
                cache.m_file_stat.emplace(path, Stat{ .m_err = errn });
            }
            return -errn;
        }

        util::StringSplitter{ ls.cout, { '\n' } }.while_next([&](Str line) {
            // skip lines too short to process
            if (line.size() < detail::minimum_path_size) {
                return;
            }

            detail::parse_file_stat(line, cache, serial, [&](detail::ParsedStat parsed) {
                auto& [path, stat] = parsed;
                filler(buf, path.data(), nullptr, 0);
            });
        });

        return 0;
    }

    int getattr(const char* path, struct stat* stbuf)
    {
        log_i({ "getattr: {:?}" }, path);

        auto& [cache, temp, serial, _] = detail::get_data();

        if (auto found = cache.m_file_stat.find(path); found == cache.m_file_stat.end()) {
            auto ls = cmd::exec_adb({ "ls", "-llad", path }, serial);
            if (ls.returncode != 0) {
                auto errn = to_errno(detail::parse_stderr(ls.cerr));
                log_e({ "getattr: failed to get attribute[{}] (err: {}) {}" }, ls.returncode, errn, ls.cerr);
                return -errn;
            }

            // to elimiate any trailing newline
            auto line = util::StringSplitter{ ls.cout, { '\n' } }.next();

            auto success = detail::parse_file_stat(*line, cache, serial, [&](auto parsed) { });

            if (not success) {
                log_e({ "getattr: failed to parse file stat [{}]" }, ls.cout);
                return -EIO;
            }
        }

        auto lock        = std::shared_lock{ cache.m_mutex };
        auto& [__, stat] = *cache.m_file_stat.find(path);

        memset(stbuf, 0, sizeof(struct stat));

        stbuf->st_ino   = 1;    // fake inode number;
        stbuf->st_mode  = static_cast<__mode_t>(stat.m_mode);
        stbuf->st_nlink = static_cast<__nlink_t>(stat.m_links);
        stbuf->st_uid   = static_cast<__uid_t>(stat.m_uid);
        stbuf->st_gid   = static_cast<__gid_t>(stat.m_gid);

        switch (stbuf->st_mode & S_IFMT) {
        case S_IFBLK:
        case S_IFCHR:
            /* TODO: implement parse_file_stat but for block and character devices */
            stbuf->st_size = 0;
            break;
        case S_IFREG:
            stbuf->st_size = stat.m_size;    //
            break;
        case S_IFSOCK:
        case S_IFIFO:
        case S_IFDIR:
        default: stbuf->st_size = 0;
        }

        stbuf->st_blksize = 512;
        stbuf->st_blocks  = (stbuf->st_size + 256) / 512;

        namespace chr = std::chrono;

        auto time = timespec{
            .tv_sec  = chr::time_point_cast<chr::seconds>(stat.m_mtime).time_since_epoch().count(),
            .tv_nsec = chr::time_point_cast<chr::nanoseconds>(stat.m_mtime).time_since_epoch().count(),
        };

        stbuf->st_atim = time;
        stbuf->st_mtim = time;
        stbuf->st_ctim = time;

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
