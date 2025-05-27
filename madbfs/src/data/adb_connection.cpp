#include "madbfs/data/adb_connection.hpp"

#include "madbfs-common/util/split.hpp"
#include "madbfs/log.hpp"
#include "madbfs/path/path.hpp"

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
     * @brief Parse the output of `stat -c '%f %h %s %u %g %X %Y %Z %n' <path>`
     */
    madbfs::Opt<madbfs::data::ParsedStat> parse_file_stat(madbfs::Str str)
    {
        return madbfs::util::split_n<8>(str, ' ').transform([](madbfs::util::SplitResult<8>&& res) {
            auto [mode_hex, hardlinks, size, uid, gid, atime, mtime, ctime] = res.result;
            return madbfs::data::ParsedStat{
                .stat = madbfs::data::Stat{
                    .links = parse_fundamental<nlink_t>(hardlinks, 10).value_or(0),
                    .size  = parse_fundamental<off_t>(size, 10).value_or(0),
                    .mtime = parse_fundamental<time_t>(mtime, 10).value_or(0),
                    .atime = parse_fundamental<time_t>(atime, 10).value_or(0),
                    .ctime = parse_fundamental<time_t>(ctime, 10).value_or(0),
                    .mode  = parse_fundamental<mode_t>(mode_hex, 16).value_or(0),
                    .uid   = parse_fundamental<uid_t>(uid, 10).value_or(0),
                    .gid   = parse_fundamental<uid_t>(gid, 10).value_or(0),
                },
                .path = get_basename(res.remainder),
            };
        });
    }

    // resolve relative path
    madbfs::String resolve_path(madbfs::path::Path parent, madbfs::Str path)
    {
        auto parents = madbfs::Vec<madbfs::Str>{};
        if (path.front() != '/') {
            parents = madbfs::util::split(parent.fullpath(), '/');
        }

        madbfs::util::StringSplitter{ path, '/' }.while_next([&](madbfs::Str str) {
            if (str == ".") {
                return;
            } else if (str == "..") {
                if (not parents.empty()) {
                    parents.pop_back();
                }
                return;
            }
            parents.push_back(str);
        });

        if (parents.empty()) {
            return "/";
        }

        auto resolved = madbfs::String{};
        for (auto path : parents) {
            resolved += '/';
            resolved += path;
        }

        return resolved;
    }

    // NOTE: somehow adb shell needs double escaping
    madbfs::String quoted(madbfs::path::Path path)
    {
        return fmt::format("\"{}\"", path.fullpath());
    }
}

namespace madbfs::data
{
    using namespace std::string_view_literals;

    AExpect<Gen<ParsedStat>> AdbConnection::statdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{
            "shell"sv,     "find"sv, Str{ qpath },
            "-maxdepth"sv, "1"sv,    "-exec"sv,
            "stat"sv,      "-c"sv,   "'%f %h %s %u %g %X %Y %Z %n'"sv,
            "{}"sv,        "+"sv,
        };

        auto out = co_await exec_async("adb", args, "", false);
        if (not out.has_value()) {
            co_return Unexpect{ out.error() };
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

        co_return generator(std::move(*out));
    }

    AExpect<Stat> AdbConnection::stat(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto cmd = Array{ "shell"sv, "stat"sv, "-c"sv, "'%f %h %s %u %g %X %Y %Z %n'"sv, Str{ qpath } };

        auto res = co_await exec_async("adb", cmd, "", true);

        co_return res.and_then([&](String out) {
            return ok_or(parse_file_stat(util::strip(out)), Errc::io_error)
                .transform([](ParsedStat parsed) { return parsed.stat; })
                .transform_error([&](auto err) {
                    log_e({ "Connection::stat: parsing stat failed [{}]" }, path.fullpath());
                    return err;
                });
        });
    }

    AExpect<path::PathBuf> AdbConnection::readlink(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "readlink"sv, Str{ qpath } };

        auto res = co_await exec_async("adb", args, "", true);

        co_return res.transform([&](String target) {
            auto target_path = resolve_path(path.parent_path(), util::strip(target));
            return path::create_buf(std::move(target_path)).value();
        });
    }

    AExpect<void> AdbConnection::touch(path::Path path, bool create)
    {
        const auto qpath = quoted(path);
        if (create) {
            const auto args = Array{ "shell"sv, "touch"sv, Str{ qpath } };
            co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
        } else {
            const auto args = Array{ "shell"sv, "touch"sv, "-c"sv, Str{ qpath } };
            co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
        }
    }

    AExpect<void> AdbConnection::mkdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "mkdir"sv, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> AdbConnection::unlink(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "rm"sv, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> AdbConnection::rmdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "rmdir"sv, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> AdbConnection::rename(path::Path from, path::Path to)
    {
        const auto qfrom = quoted(from);
        const auto qto   = quoted(to);
        const auto args  = Array{ "shell"sv, "mv"sv, Str{ qfrom }, Str{ qto } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> AdbConnection::truncate(path::Path path, off_t size)
    {
        const auto qpath = quoted(path);
        const auto sizes = fmt::format("{}", size);
        const auto args  = Array{ "shell"sv, "truncate"sv, "-s"sv, Str{ sizes }, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<u64> AdbConnection::open(path::Path /* path */, int /* flags */)
    {
        /*
         * Since we're using a streaming approach to read/write files, there is no need to do open or
         * close operation on the file. The file is opened when we read or write to it, and closed after
         * those operation complete on the device.
         *
         * Thus, there is really no need to do anything here.
         */

        co_return m_counter.fetch_add(1, std::memory_order::relaxed) + 1;
    }

    AExpect<usize> AdbConnection::read(path::Path path, Span<char> out, off_t offset)
    {
        const auto skip  = fmt::format("skip={}", offset);
        const auto count = fmt::format("count={}", out.size());
        const auto iff   = fmt::format("if=\"{}\"", path.fullpath());

        // bs is skipped, relies on `count_bytes`: https://stackoverflow.com/a/40792605/16506263

        const auto args = Array{
            "shell"sv,
            "dd"sv,
            "iflag=skip_bytes,count_bytes"sv,    // https://stackoverflow.com/a/40792605/16506263
            Str{ skip },
            Str{ count },
            Str{ iff },
        };

        co_return (co_await exec_async("adb", args, "", true)).transform([&](String str) {
            auto size = std::min(str.size(), out.size());
            std::copy_n(str.data(), size, out.data());
            return size;
        });
    }

    AExpect<usize> AdbConnection::write(path::Path path, Span<const char> in, off_t offset)
    {
        const auto seek = fmt::format("seek={}", offset);
        const auto off  = fmt::format("of=\"{}\"", path.fullpath());

        const auto args = Array{
            "shell"sv,
            "dd"sv,
            "oflag=seek_bytes"sv,    // same as above, though I don't know if this is really needed
            "conv=notrunc"sv,        // https://unix.stackexchange.com/a/146923
            Str{ seek },
            Str{ off },
        };

        auto in_str = Str{ in.data(), in.size() };
        co_return (co_await exec_async("adb", args, in_str, true)).transform([&](auto&&) {
            return in_str.size();    // assume all the data is written to device
        });
    }

    AExpect<void> AdbConnection::flush(path::Path /* path */)
    {
        /*
         * The streaming approach is immediate, so there is no need to flush the file. At least for the
         * moment. In the future we might want to implement caching once again.
         */

        return {};
    }

    AExpect<void> AdbConnection::release(path::Path /* path */)
    {
        /*
         * The reason this part is a no-op is the same as the open function. We don't need to do any
         * open or close operation on the file.
         */

        return {};
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
        const auto iff   = fmt::format("if=\"{}\"", in.fullpath());
        const auto seek  = fmt::format("seek={}", out_off);
        const auto off   = fmt::format("of=\"{}\"", out.fullpath());

        const auto args = Array{
            "shell"sv,
            "dd"sv,
            "iflag=skip_bytes,count_bytes"sv,    // https://stackoverflow.com/a/40792605/16506263
            Str{ skip },
            Str{ count },
            Str{ iff },
            "oflag=seek_bytes"sv,    // same as above, though I don't know if this is really needed
            "conv=notrunc"sv,        // https://unix.stackexchange.com/a/146923
            Str{ seek },
            Str{ off },
        };

        // example output
        /*
         * conv=4
         * 1024+0 records in
         * 1024+0 records out
         * 1048576 bytes (1.0 M) copied, 0.054401 s, 18 M/s
         */

        co_return (co_await exec_async("adb", args, "", true, true)).transform([&](String str) {
            log_d({ "copy_file_range: {:?}" }, str);
            return util::split_n<4>(str, '\n')
                .and_then([](util::SplitResult<4> r) { return util::split_n<1>(r.result[3], ' '); })
                .and_then([](util::SplitResult<1> r) { return parse_fundamental<usize>(r.result[0], 10); })
                .value_or(0);
        });
    }
}
