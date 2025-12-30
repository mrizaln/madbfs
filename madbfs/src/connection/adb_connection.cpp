#include "madbfs/connection/adb_connection.hpp"

#include "madbfs/cmd.hpp"
#include "madbfs/path.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/split.hpp>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace
{
    /**
     * @brief Parse integral types from string.
     *
     * @tparam T The type to be parsed.
     *
     * @param str Input string.
     * @param base Integer base to use.
     *
     * @return Parsed integer on success else `std::nullopt`.
     */
    template <std::integral T>
    constexpr madbfs::Opt<T> parse_integral(madbfs::Str str, int base)
    {
        auto t         = T{};
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), t, base);
        if (ptr != str.data() + str.size() or ec != madbfs::Errc{}) {
            return {};
        }
        return t;
    }

    /**
     * @brief Strip directory and suffix from path.
     *
     * @param path Path string.
     *
     * @return Stripped path (aka filename only).
     */
    madbfs::Str get_basename(madbfs::Str path)
    {
        if (path != "/") {
            auto found = madbfs::sr::find(path | madbfs::sv::reverse, '/');
            if (found != path.rend()) {    // no '/' means already basename
                return madbfs::Str{ found.base(), path.end() };
            }
        }
        return path;
    }

    /**
     * @brief Parse date in the form "2025-06-04 12:09:05.450071907 +0700"
     *
     * @param date Date string.
     *
     * @return Time represented in `timespec`.
     */
    timespec parse_date(madbfs::Str date)
    {
        // apparently istringstream only have the overload for string_view from C++26 onwards. damn it!
        auto in = std::istringstream{ madbfs::String{ date } };    // unnecessary copy... :(
        auto tp = std::chrono::sys_time<std::chrono::nanoseconds>{};

        in >> std::chrono::parse("%F %T %z", tp);
        if (in.fail()) {
            madbfs::log_d("{}: fail to parse {:?}", __func__, date);
        }

        auto secs  = std::chrono::time_point_cast<std::chrono::seconds>(tp);
        auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(tp - secs);

        return { secs.time_since_epoch().count(), nsecs.count() };
    }

    /**
     * @brief Parse the output of `stat -c '%f|%h|%s|%u|%g|%x|%y|%z|%n' <path>`
     *
     * @paran str Input string.
     *
     * @return Parsed stat if success else `std::nullopt`.
     */
    madbfs::Opt<madbfs::connection::ParsedStat> parse_file_stat(madbfs::Str str)
    {
        return madbfs::util::split_n<8>(str, '|').transform([](madbfs::util::SplitResult<8>&& res) {
            auto [mode_hex, hardlinks, size, uid, gid, atime, mtime, ctime] = res.result;
            return madbfs::connection::ParsedStat{
                .stat = madbfs::data::Stat{
                    .links = parse_integral<nlink_t>(hardlinks, 10).value_or(0),
                    .size  = parse_integral<off_t>(size, 10).value_or(0),
                    .mtime = parse_date(mtime),
                    .atime = parse_date(atime),
                    .ctime = parse_date(ctime),
                    .mode  = parse_integral<mode_t>(mode_hex, 16).value_or(0),
                    .uid   = parse_integral<uid_t>(uid, 10).value_or(0),
                    .gid   = parse_integral<uid_t>(gid, 10).value_or(0),
                },
                .name = get_basename(res.remainder),
            };
        });
    }

    /**
     * @brief Create a new quoted path.
     *
     * @param path Path to be quoted
     *
     * @return Quoted path.
     *
     *  NOTE: adb shell apparently needs double escaping
     */
    madbfs::String quote(madbfs::path::Path path)
    {
        return std::format("\"{}\"", path);
    }
}

namespace madbfs::connection
{
    AExpect<Gen<ParsedStat>> AdbConnection::statdir(path::Path path)
    {
        const auto qpath = quote(path);
        const auto cmd   = std::to_array<Str>({
            "adb",
            "shell",
            "find",
            qpath,
            "-maxdepth",
            "1",
            "-exec",
            "stat",
            "-c",
            "'%f|%h|%s|%u|%g|%x|%y|%z|%n'",
            "{}",
            "+",
        });

        auto res = co_await cmd::exec(cmd, "", false, false);
        if (not res.has_value()) {
            co_return Unexpect{ res.error() };
        }

        auto generator = [](String out) -> Gen<ParsedStat> {
            auto lines  = util::StringSplitter{ out, '\n' };
            std::ignore = lines.next();    // ignore first line, the directory itself
            while (auto line = lines.next()) {
                auto parsed = parse_file_stat(util::strip(*line));
                if (not parsed.has_value()) {
                    continue;
                }
                co_yield std::move(parsed).value();
            }
        };

        co_return generator(std::move(*res));
    }

    AExpect<data::Stat> AdbConnection::stat(path::Path path)
    {
        auto res = co_await cmd::exec(
            { "adb", "shell", "stat", "-c", "'%f|%h|%s|%u|%g|%x|%y|%z|%n'", quote(path) }
        );

        co_return res.and_then([&](Str out) {
            return ok_or(parse_file_stat(util::strip(out)), Errc::io_error)
                .transform([](ParsedStat parsed) { return parsed.stat; })
                .transform_error([&](auto err) {
                    log_e("Connection::stat: parsing stat failed [{}]", path);
                    return err;
                });
        });
    }

    AExpect<String> AdbConnection::readlink(path::Path path)
    {
        auto res = co_await cmd::exec({ "adb", "shell", "readlink", quote(path) });
        co_return res.transform([&](Str target) { return String{ util::strip(target) }; });
    }

    AExpect<void> AdbConnection::mknod(path::Path path, mode_t /* mode */, dev_t /* dev */)
    {
        auto res = co_await cmd::exec({ "adb", "shell", "touch", quote(path) });
        co_return res.transform(sink_void);
    }

    AExpect<void> AdbConnection::mkdir(path::Path path, mode_t /* mode */)
    {
        auto res = co_await cmd::exec({ "adb", "shell", "mkdir", quote(path) });
        co_return res.transform(sink_void);
    }

    AExpect<void> AdbConnection::unlink(path::Path path)
    {
        auto res = co_await cmd::exec({ "adb", "shell", "rm", quote(path) });
        co_return res.transform(sink_void);
    }

    AExpect<void> AdbConnection::rmdir(path::Path path)
    {
        auto res = co_await cmd::exec({ "adb", "shell", "rmdir", quote(path) });
        co_return res.transform(sink_void);
    }

    AExpect<void> AdbConnection::rename(path::Path from, path::Path to, u32 flags)
    {
        if (flags == RENAME_EXCHANGE) {
            // NOTE: there is no --exchange flag on Android's mv
            // NOTE: renameat2 returns EINVAL when the fs doesn't support exchange operation see man rename(2)
            co_return Unexpect{ Errc::invalid_argument };
        } else if (flags == RENAME_NOREPLACE) {
            auto res = co_await cmd::exec({ "adb", "shell", "mv", "-n", quote(from), quote(to) });
            co_return res.transform(sink_void);
        } else {
            auto res = co_await cmd::exec({ "adb", "shell", "mv", quote(from), quote(to) });
            co_return res.transform(sink_void);
        }
    }

    AExpect<void> AdbConnection::truncate(path::Path path, off_t size)
    {
        const auto size_str = std::format("{}", size);

        auto res = co_await cmd::exec({ "adb", "shell", "truncate", "-s", size_str, quote(path) });
        co_return res.transform(sink_void);
    }

    AExpect<void> AdbConnection::utimens(path::Path path, timespec atime, timespec mtime)
    {
        for (auto [time, flag] : { Pair{ atime, "-a" }, Pair{ mtime, "-m" } }) {
            switch (time.tv_nsec) {
            case UTIME_OMIT: continue;
            case UTIME_NOW: {
                auto res = co_await cmd::exec({ "adb", "shell", "touch", "-c", flag, quote(path) });
                if (not res) {
                    co_return Unexpect{ res.error() };
                }
            } break;
            default:
                auto tm = std::tm{};
                if (::localtime_r(&time.tv_sec, &tm) == nullptr) {    // localtime_r is C23 feature
                    co_return Unexpect{ Errc::invalid_argument };
                }

                const auto sys = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                const auto t   = std::format("{:%Y%m%d%H%M.%S}{}", sys, time.tv_nsec);

                log_i("{}: utimens to {}", __func__, t);

                auto res = co_await cmd::exec({ "adb", "shell", "touch", "-c", flag, "-t", t, quote(path) });
                if (not res) {
                    co_return Unexpect{ res.error() };
                }
            }
        }

        co_return Expect<void>{};
    }

    AExpect<usize> AdbConnection::copy_file_range(
        path::Path in,
        off_t      in_off,
        path::Path out,
        off_t      out_off,
        usize      size
    )
    {
        const auto skip  = std::format("skip={}", in_off);
        const auto count = std::format("count={}", size);
        const auto ifile = std::format("if=\"{}\"", in);
        const auto seek  = std::format("seek={}", out_off);
        const auto ofile = std::format("of=\"{}\"", out);

        // count_bytes: https://stackoverflow.com/a/40792605/16506263
        // notrunc    : https://unix.stackexchange.com/a/146923
        const auto cmd = std::to_array<Str>({
            "adb",
            "shell",
            "dd",
            "iflag=skip_bytes,count_bytes",
            skip,
            count,
            ifile,
            "oflag=seek_bytes",
            "conv=notrunc",
            seek,
            ofile,
        });

        auto res = co_await cmd::exec(cmd, "", true, true);

        // example output
        /*
         * conv=4
         * 1024+0 records in
         * 1024+0 records out
         * 1048576 bytes (1.0 M) copied, 0.054401 s, 18 M/s
         */

        co_return res.transform([&](Str str) {
            log_d("copy_file_range: {:?}", str);
            return util::split_n<4>(str, '\n')
                .and_then([](util::SplitResult<4> r) { return util::split_n<1>(r.result[3], ' '); })
                .and_then([](util::SplitResult<1> r) { return parse_integral<usize>(r.result[0], 10); })
                .value_or(0);
        });
    }

    AExpect<u64> AdbConnection::open(path::Path path, data::OpenMode /* mode */)
    {
        auto fd = ++m_fd_counter;
        m_fd_map.emplace(fd, path.owned());    // this is expensive, try to find a way to deduplicate the path
        co_return fd;
    }

    AExpect<void> AdbConnection::close(u64 fd)
    {
        auto entry = m_fd_map.find(fd);
        if (entry == m_fd_map.end()) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        m_fd_map.erase(entry);
        co_return Expect<void>{};
    }

    AExpect<usize> AdbConnection::read(u64 fd, Span<char> out, off_t offset)
    {
        auto entry = m_fd_map.find(fd);
        if (entry == m_fd_map.end()) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto path = entry->second.view();

        const auto skip  = std::format("skip={}", offset);
        const auto count = std::format("count={}", out.size());
        const auto ifile = std::format("if=\"{}\"", path);

        // `bs` is skipped, relies on `count_bytes`: https://stackoverflow.com/a/40792605/16506263
        auto res = co_await cmd::exec(
            { "adb", "shell", "dd", "iflag=skip_bytes,count_bytes", skip, count, ifile }
        );

        co_return res.transform([&](Str str) {
            auto size = std::min(str.size(), out.size());
            std::copy_n(str.data(), size, out.data());
            return size;
        });
    }

    AExpect<usize> AdbConnection::write(u64 fd, Span<const char> in, off_t offset)
    {
        auto entry = m_fd_map.find(fd);
        if (entry == m_fd_map.end()) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto path = entry->second.view();

        const auto seek  = std::format("seek={}", offset);
        const auto ofile = std::format("of=\"{}\"", path);

        auto in_str = Str{ in.data(), in.size() };

        // `notrunc` flag is necessary to prevent truncating file: https://unix.stackexchange.com/a/146923
        auto res = co_await cmd::exec(
            { "adb", "shell", "dd", "oflag=seek_bytes", "conv=notrunc", seek, ofile }, in_str
        );

        // assume all the data is written to device on success
        co_return res.transform([&](auto&&) { return in_str.size(); });
    }
}
