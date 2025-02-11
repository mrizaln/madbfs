#include "cmd.hpp"
#include "common.hpp"
#include "util.hpp"
#include "adbfsm.hpp"

#include <fuse.h>
#include <fcntl.h>

#include <cstring>
#include <charconv>
#include <cassert>

namespace detail
{
    using namespace adbfsm;

    static constexpr usize minimum_path_size   = 3;
    static constexpr int   default_gid         = 98;
    static constexpr int   default_uid         = 98;
    static constexpr Str   no_device           = "adb: no devices/emulators found";
    static constexpr Str   permission_denied   = "Permission denied";
    static constexpr Str   no_such_file_or_dir = "No such file or directory";
    static constexpr Str   not_a_directory     = "Not a directory";
    static constexpr Str   inaccessible        = "inaccessible or not found";
    static constexpr Str   bash_escapes        = " \t\n\'\"\\`$()<>;&|*?[#~=%";

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

    AdbfsmData& get_data()
    {
        auto ctx = fuse_get_context()->private_data;
        assert(ctx != nullptr);
        return *static_cast<AdbfsmData*>(ctx);
    }

    std::string copy_replace(const char* str, char replace, char with)
    {
        auto  new_str = std::string{};
        auto* ch      = str;
        while (*ch != '\0') {
            *ch == replace ? new_str.push_back(with) : new_str.push_back(*ch);
            ++ch;
        }
        return new_str;
    }

    std::string copy_escape(const char* str, Str escapes = bash_escapes)
    {
        auto  new_str = std::string{};
        auto* ch      = str;
        while (*ch != '\0') {
            if (escapes.find(*ch) != std::string::npos) {
                new_str.push_back('\\');
            }
            new_str.push_back(*ch);
            ++ch;
        }
        return new_str;
    }

    std::string copy_replace_escape(const char* str, char replace, char with, Str escapes = bash_escapes)
    {
        auto  new_str = std::string{};
        auto* ch      = str;
        while (*ch != '\0') {
            if (escapes.find(*ch) != std::string::npos) {
                new_str.push_back('\\');
            }
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
        auto splitter = util::StringSplitter{ str, '\n' };
        while (auto line = splitter.next()) {
            if (*line == no_device) {
                return Error::NoDev;
            }

            auto rev = std::string{ line->rbegin(), line->rend() };
            auto err = util::StringSplitter{ rev, ':' }.next();
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
            } else if (eq(*err, inaccessible)) {
                return Error::Inaccessible;
            }
        }

        return Error::Unknown;
    }

    int parse_and_log_cmd_err(const cmd::Out& out, Str fun, Str msg)
    {
        auto errn = to_errno(parse_stderr(out.cerr));
        log_e({ "{}: {} [{}] (errno: {}) {}" }, fun, msg, out.returncode, errn, out.cerr);
        return errn;
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

        auto gid = cmd::exec_adb_shell({ "id", "-g", std::string{ group } }, serial);
        if (gid.returncode != 0) {
            log_e({ "get_gid: failed to get gid for [{}] (err: {}) {}" }, group, gid.returncode, gid.cerr);
        }

        auto lock     = std::unique_lock{ cache.m_mutex };
        auto real_gid = parse_fundamental<int>(util::strip(gid.cout, '\n')).value_or(default_gid);

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

        auto uid = cmd::exec_adb_shell({ "id", "-u", std::string{ user } }, serial);
        if (uid.returncode != 0) {
            log_e({ "get_uid: failed to get uid for [{}] (err: {}) {}" }, user, uid.returncode, uid.cerr);
        }

        auto lock     = std::unique_lock{ cache.m_mutex };
        auto real_uid = parse_fundamental<int>(util::strip(uid.cout, '\n')).value_or(default_uid);

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
    bool parse_file_stat(Str parent, Str str, Cache& cache, Str serial, Fn&& fn)
    {
        auto result = util::split_n<8>(str, ' ');
        if (not result) {
            return false;
        }

        auto [to_be_stat, remainder] = *result;

        auto path = remainder;
        auto link = std::string{};

        if (auto arrow = remainder.find(" -> "); arrow != std::string::npos) {
            path = remainder.substr(0, arrow);

            auto to_be_link  = remainder.substr(arrow + 4);
            auto num_slashes = sr::count(path | sv::drop(1), '/');

            auto pos = 0_usize;
            while (to_be_link[pos] == '/') {
                ++pos;
            }
            for (; num_slashes; --num_slashes) {
                link.append("../");
            }

            link.append(to_be_link.data() + pos, to_be_link.size() - pos);
        }

        if (to_be_stat[0].find_first_of('?') != std::string::npos) {
            log_e({ "parse_file_stat: failed, file contains unparsable data [{}]" }, to_be_stat[0]);
            return false;
        }

        {
            auto lock        = std::shared_lock{ cache.m_mutex };
            auto actual_path = fs::path{ parent } / path;
            if (auto found = cache.m_file_stat.find(actual_path.c_str()); found != cache.m_file_stat.end()) {
                auto& [_, stat] = *found;
                auto basename   = actual_path.filename();
                fn(ParsedStat{ .m_path = basename.c_str(), .m_stat = stat });
                return true;
            }
        }

        auto stat = Stat{
            .m_mode    = parse_mode(to_be_stat[0]),
            .m_links   = parse_fundamental<int>(to_be_stat[1]).value(),
            .m_uid     = get_uid(to_be_stat[2], cache, serial),
            .m_gid     = get_gid(to_be_stat[3], cache, serial),
            .m_size    = parse_fundamental<int>(to_be_stat[4]).value_or(0),
            .m_mtime   = parse_time(to_be_stat[5], to_be_stat[6], to_be_stat[7]).value(),
            .m_age     = Clock::now(),
            .m_link_to = link,
        };

        if ((stat.m_mode & S_IFMT) == S_IFLNK and link.empty()) {
            log_c({ "parse_file_stat: link is empty for [{}] when it should not be" }, path);
        }

        auto it          = cache.m_file_stat.begin();
        auto actual_path = fs::path{ parent } / path;
        {
            auto lock = std::unique_lock{ cache.m_mutex };
            it        = cache.m_file_stat.emplace(actual_path, stat).first;
        }

        auto basename = actual_path.filename();
        fn(ParsedStat{ .m_path = basename.c_str(), .m_stat = it->second });
        return true;
    }

    cmd::Out adb_rescan_file(Str path, Str serial)
    {
        auto file   = fmt::format("'file://{}'", path);
        auto intent = "android.intent.action.MEDIA_SCANNER_SCAN_FILE";
        auto cmd    = cmd::Cmd{ "am", "broadcast", "-a", intent, "-d", file };
        return cmd::exec_adb_shell(std::move(cmd), serial);
    }
}

namespace adbfsm
{
    void destroy(void* private_data)
    {
        auto& data = *static_cast<AdbfsmData*>(private_data);
        fs::remove_all(data.m_dir);
        adbfsm::log_i({ "cleaned up temporary directory: {:?}" }, data.m_dir.c_str());
    }

    int readdir(
        const char*                      path,
        void*                            buf,
        fuse_fill_dir_t                  filler,
        [[maybe_unused]] off_t           offset,
        [[maybe_unused]] fuse_file_info* fi
    )
    {
        log_i({ "readdir: {:?}" }, path);
        auto& [cache, temp, serial, readdir_flag, _] = detail::get_data();
        readdir_flag.wait(true);

        if (auto found = cache.m_file_stat.find(path); found != cache.m_file_stat.end()) {
            auto& [path, stat] = *found;
            if (stat.m_err != 0) {
                return -stat.m_err;
            }
        }

        auto ls = cmd::exec_adb_shell({ "ls", "-lla", detail::copy_escape(path) }, serial);
        if (ls.returncode != 0) {
            auto errn = detail::parse_and_log_cmd_err(ls, "readdir", "failed to list directory");
            cache.m_file_stat.emplace(path, Stat{ .m_err = errn });
            return -errn;
        }

        util::StringSplitter{ ls.cout, '\n' }.while_next([&](Str line) {
            if (line.size() < detail::minimum_path_size) {    // skip lines too short to process
                return;
            }
            detail::parse_file_stat(path, line, cache, serial, [&](detail::ParsedStat parsed) {
                auto& [path, stat] = parsed;
                filler(buf, path.data(), nullptr, 0);
            });
        });

        readdir_flag = false;
        readdir_flag.notify_all();

        return 0;
    }

    int getattr(const char* path, struct stat* stbuf)
    {
        log_i({ "getattr: {:?}" }, path);
        auto& [cache, temp, serial, readdir_flag, _] = detail::get_data();
        readdir_flag.wait(true);

        if (auto found = cache.m_file_stat.find(path); found == cache.m_file_stat.end()) {
            auto ls = cmd::exec_adb_shell({ "ls", "-llad", detail::copy_escape(path) }, serial);
            if (ls.returncode != 0) {
                auto errn = detail::parse_and_log_cmd_err(ls, "getattr", "failed to get file info");
                cache.m_file_stat.emplace(path, Stat{ .m_err = errn });
                return -errn;
            }

            auto success = detail::parse_file_stat("/", util::strip(ls.cout), cache, serial, [&](auto) { });
            if (not success) {
                log_e({ "getattr: failed to parse file stat [{}]" }, ls.cout);
                return -EIO;
            }
        }

        auto lock        = std::shared_lock{ cache.m_mutex };
        auto& [__, stat] = *cache.m_file_stat.find(path);

        std::memset(stbuf, 0, sizeof(struct stat));

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

    int access([[maybe_unused]] const char* path, [[maybe_unused]] int mask)
    {
        // NOTE: empty
        return 0;
    }

    int open(const char* path, fuse_file_info* fi)
    {
        log_i({ "open: {:?}" }, path);
        auto& [cache, temp, serial, readdir_flag, rescan] = detail::get_data();

        auto path_str   = detail::copy_escape(path);
        auto local_path = temp / detail::copy_replace(path_str.c_str(), '/', '-');

        if (fs::exists(local_path)) {
            fi->fh = static_cast<u64>(::open(local_path.c_str(), fi->flags));
            return 0;
        }

        if (not cache.m_file_truncated.contains(path)) {
            auto pull = cmd::exec_adb({ "pull", std::move(path_str), local_path }, serial);
            if (pull.returncode != 0) {
                auto errn = detail::parse_and_log_cmd_err(pull, "open", "failed to pull file");
                cache.m_file_stat.emplace(path, Stat{ .m_err = errn });
                return -errn;
            }
        }

        fi->fh = static_cast<u64>(::open(local_path.c_str(), fi->flags));
        return 0;
    }

    int flush(const char* path, fuse_file_info* fi)
    {
        log_c({ "flush [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int release(const char* path, fuse_file_info* fi)
    {
        log_i({ "release: {}" }, path);
        auto& [cache, temp, serial, readdir_flag, rescan] = detail::get_data();

        auto local_path = temp / detail::copy_replace_escape(path, '/', '-');

        // close file
        auto fd = static_cast<i32>(fi->fh);
        cache.m_file_pending_write.erase(fd);
        auto res = ::close(fd);
        if (res == -1) {
            log_e({ "release: failed to close file [{}] (errno: {})" }, path, errno);
            return -errno;
        }

        // remove local copy
        // TODO: use heuristic to whether remove this local copy or not
        res = ::unlink(local_path.c_str());
        if (res == -1) {
            log_e({ "release: failed to unlink file [{}] (errno: {})" }, path, errno);
            return -errno;
        }

        return 0;
    }

    int read(const char* path, char* buf, size_t size, off_t offset, fuse_file_info* fi)
    {
        log_i({ "read: {}" }, path);
        auto& [cache, temp, serial, readdir_flag, rescan] = detail::get_data();

        auto fd = static_cast<i32>(fi->fh);
        if (fd == -1) {
            log_e({ "read: invalid file descriptor" });
            return -EBADF;
        }

        auto res = pread(fd, buf, size, offset);
        if (res == -1) {
            log_e({ "read: failed to read file [{}] (errno: {})" }, path, errno);
            return -errno;
        }

        return static_cast<i32>(res);
    }

    int write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi)
    {
        log_c({ "write [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int utimens(const char* path, const timespec ts[2])
    {
        log_i({ "utimens: {:?}" }, path);
        auto& [cache, temp, serial, readdir_flag, rescan] = detail::get_data();
        readdir_flag.wait(true);

        // TODO: use the timedata from ts instead of the current time
        auto touch = cmd::exec_adb_shell({ "touch", detail::copy_escape(path) }, serial);
        if (touch.returncode != 0) {
            auto errn = detail::parse_and_log_cmd_err(touch, "utimens", "failed to touch file");
            cache.m_file_stat.emplace(path, Stat{ .m_err = errn });
            return -errn;
        }

        // If we forgot to mount -o rescan then we can remount and touch to trigger the scan.
        if (rescan) {
            auto out = detail::adb_rescan_file(path, serial);
            if (out.returncode != 0) {
                return -to_errno(detail::parse_stderr(out.cerr));
            }
        }

        return 0;
    }

    int truncate(const char* path, off_t size)
    {
        log_c({ "truncate [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int mknod(const char* path, mode_t mode, dev_t rdev)
    {
        log_c({ "mknod [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int mkdir(const char* path, mode_t mode)
    {
        log_c({ "mkdir [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int rename(const char* from, const char* to)
    {
        log_c({ "rename [unimplemented]: {} -> {}" }, from, to);
        return -ENOSYS;
    }

    int rmdir(const char* path)
    {
        log_c({ "rmdir [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int unlink(const char* path)
    {
        log_c({ "unlink [unimplemented]: {}" }, path);
        return -ENOSYS;
    }

    int readlink(const char* path, char* buf, size_t size)
    {
        log_i({ "readlink: {:?}" }, path);
        auto& [cache, temp, serial, readdir_flag, _] = detail::get_data();
        readdir_flag.wait(true);

        if (auto found = cache.m_file_stat.find(path); found == cache.m_file_stat.end()) {
            auto ls = cmd::exec_adb_shell({ "ls", "-llad", detail::copy_escape(path) }, serial);
            if (ls.returncode != 0) {
                auto errn = detail::parse_and_log_cmd_err(ls, "readlink", "failed to read file info");
                cache.m_file_stat.emplace(path, Stat{ .m_err = errn });
                return -errn;
            }

            auto success = detail::parse_file_stat("/", util::strip(ls.cout), cache, serial, [&](auto) { });
            if (not success) {
                log_e({ "readdir: failed to parse file stat [{}]" }, ls.cout);
                return -EIO;
            }
        }

        auto lock        = std::shared_lock{ cache.m_mutex };
        auto& [__, stat] = *cache.m_file_stat.find(path);

        if ((stat.m_mode & S_IFMT) != S_IFLNK) {
            log_e({ "readlink: [{}] is not a link" }, path);
            return -EINVAL;
        }

        if (stat.m_link_to.size() >= size) {
            log_e({ "readlink: buffer too small for link [{}]" }, path);
            return -ENOSYS;
        }

        log_d({ "readlink: [{}] -> [{}]" }, path, stat.m_link_to);

        std::memcpy(buf, stat.m_link_to.c_str(), std::min(size, stat.m_link_to.size() + 1));

        return 0;
    }
}
