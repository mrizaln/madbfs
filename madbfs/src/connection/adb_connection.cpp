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
    template <typename T>
        requires std::is_fundamental_v<T>
    constexpr madbfs::Opt<T> parse_fundamental(madbfs::Str str, int base)
    {
        auto t         = T{};
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), t, base);
        if (ptr != str.data() + str.size() or ec != madbfs::Errc{}) {
            return {};
        }
        return t;
    }

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
     */
    madbfs::Opt<madbfs::connection::ParsedStat> parse_file_stat(madbfs::Str str)
    {
        return madbfs::util::split_n<8>(str, '|').transform([](madbfs::util::SplitResult<8>&& res) {
            auto [mode_hex, hardlinks, size, uid, gid, atime, mtime, ctime] = res.result;
            return madbfs::connection::ParsedStat{
                .stat = madbfs::data::Stat{
                    .links = parse_fundamental<nlink_t>(hardlinks, 10).value_or(0),
                    .size  = parse_fundamental<off_t>(size, 10).value_or(0),
                    .mtime = parse_date(mtime),
                    .atime = parse_date(atime),
                    .ctime = parse_date(ctime),
                    .mode  = parse_fundamental<mode_t>(mode_hex, 16).value_or(0),
                    .uid   = parse_fundamental<uid_t>(uid, 10).value_or(0),
                    .gid   = parse_fundamental<uid_t>(gid, 10).value_or(0),
                },
                .path = get_basename(res.remainder),
            };
        });
    }

    // NOTE: somehow adb shell needs double escaping
    madbfs::String quote(madbfs::path::Path path)
    {
        return fmt::format("\"{}\"", path.fullpath());
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
                    log_e("Connection::stat: parsing stat failed [{}]", path.fullpath());
                    return err;
                });
        });
    }

    AExpect<path::PathBuf> AdbConnection::readlink(path::Path path)
    {
        auto res = co_await cmd::exec({ "adb", "shell", "readlink", quote(path) });

        co_return res.transform([&](Str target) {
            return path::resolve(path.parent_path(), util::strip(target));
        });
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
        const auto size_str = fmt::format("{}", size);

        auto res = co_await cmd::exec({ "adb", "shell", "truncate", "-s", size_str, quote(path) });
        co_return res.transform(sink_void);
    }

    AExpect<usize> AdbConnection::read(path::Path path, Span<char> out, off_t offset)
    {
        const auto skip  = fmt::format("skip={}", offset);
        const auto count = fmt::format("count={}", out.size());
        const auto ifile = fmt::format("if=\"{}\"", path.fullpath());

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

    AExpect<usize> AdbConnection::write(path::Path path, Span<const char> in, off_t offset)
    {
        const auto seek  = fmt::format("seek={}", offset);
        const auto ofile = fmt::format("of=\"{}\"", path.fullpath());

        auto in_str = Str{ in.data(), in.size() };

        // `notrunc` flag is necessary to prevent truncating file: https://unix.stackexchange.com/a/146923
        auto res = co_await cmd::exec(
            { "adb", "shell", "dd", "oflag=seek_bytes", "conv=notrunc", seek, ofile }, in_str
        );

        // assume all the data is written to device on success
        co_return res.transform([&](auto&&) { return in_str.size(); });
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

                const auto str = fmt::format("{:%Y%m%d%H%M.%S}{}", tm, time.tv_nsec);
                log_i("{}: utimens to {}", __func__, str);

                auto res = co_await cmd::exec({ "adb", "shell", "touch", "-c", flag, "-t", str, quote(path) }
                );
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
        const auto skip  = fmt::format("skip={}", in_off);
        const auto count = fmt::format("count={}", size);
        const auto ifile = fmt::format("if=\"{}\"", in.fullpath());
        const auto seek  = fmt::format("seek={}", out_off);
        const auto ofile = fmt::format("of=\"{}\"", out.fullpath());

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
                .and_then([](util::SplitResult<1> r) { return parse_fundamental<usize>(r.result[0], 10); })
                .value_or(0);
        });
    }
}
