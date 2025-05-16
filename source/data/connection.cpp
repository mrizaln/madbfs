#include "adbfsm/async/async.hpp"
#include "adbfsm/log.hpp"
#include "adbfsm/util/split.hpp"

#include "adbfsm/data/connection.hpp"
#include "adbfsm/path/path.hpp"

#define BOOST_PROCESS_VERSION 2
#include <boost/process.hpp>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace bp = boost::process::v2;

namespace
{
    static constexpr adbfsm::Str no_device           = "adb: no devices/emulators found";
    static constexpr adbfsm::Str device_offline      = "adb: device offline";
    static constexpr adbfsm::Str permission_denied   = " Permission denied";
    static constexpr adbfsm::Str no_such_file_or_dir = " No such file or directory";
    static constexpr adbfsm::Str not_a_directory     = " Not a directory";
    static constexpr adbfsm::Str inaccessible        = " inaccessible or not found";
    static constexpr adbfsm::Str read_only           = " Read-only file system";

    enum class Error
    {
        Unknown,
        NoDev,
        PermDenied,
        NoSuchFileOrDir,
        NotADir,
        Inaccessible,
        ReadOnly,
        TryAgain,
    };

    adbfsm::Str get_no_dev_serial()
    {
        if (auto* serial = std::getenv("ANDROID_SERIAL"); serial != nullptr) {
            static auto no_dev_serial = fmt::format("adb: device '{}' not found", serial);
            return no_dev_serial;
        }
        return {};
    }

    adbfsm::Errc to_errc(Error err)
    {
        switch (err) {
        case Error::Unknown: return adbfsm::Errc::io_error;
        case Error::NoDev: return adbfsm::Errc::no_such_device;
        case Error::PermDenied: return adbfsm::Errc::permission_denied;
        case Error::NoSuchFileOrDir: return adbfsm::Errc::no_such_file_or_directory;
        case Error::NotADir: return adbfsm::Errc::not_a_directory;
        case Error::Inaccessible: return adbfsm::Errc::operation_not_supported;    // program not accessible
        case Error::ReadOnly: return adbfsm::Errc::read_only_file_system;
        case Error::TryAgain: return adbfsm::Errc::resource_unavailable_try_again;
        default: std::terminate();
        }
    }

    Error parse_stderr(adbfsm::Str str)
    {
        auto splitter = adbfsm::util::StringSplitter{ str, '\n' };
        while (auto line = splitter.next()) {
            if (*line == no_device or *line == device_offline) {
                return Error::NoDev;
            } else if (*line == get_no_dev_serial()) {
                return Error::TryAgain;
            }

            auto rev       = adbfsm::String{ line->rbegin(), line->rend() };
            auto rev_strip = adbfsm::util::strip(rev);
            auto err       = adbfsm::util::StringSplitter{ rev_strip, ':' }.next();
            if (not err) {
                continue;
            }

            auto eq = [&](auto rhs) { return adbfsm::sr::equal(*err, rhs | adbfsm::sv::reverse); };

            // clang-format off
            if      (eq(permission_denied))   return Error::PermDenied;
            else if (eq(no_such_file_or_dir)) return Error::NoSuchFileOrDir;
            else if (eq(not_a_directory))     return Error::NotADir;
            else if (eq(inaccessible))        return Error::Inaccessible;
            else if (eq(read_only))           return Error::ReadOnly;
            else                              return Error::Unknown;
            // clang-format on
        }

        return Error::Unknown;
    }

    adbfsm::Await<boost::system::error_code> drain_pipe(adbfsm::async::pipe::Read& rpipe, adbfsm::String& out)
    {
        out.clear();

        auto tmp = std::array<char, 1024>{};
        auto eof = false;

        while (not eof) {
            auto tmp_read = 0uz;
            while (tmp_read < tmp.size()) {
                auto buf = adbfsm::async::buffer(tmp.data() + tmp_read, tmp.size() - tmp_read);
                auto res = co_await rpipe.async_read_some(buf);
                if (not res and res == boost::asio::error::eof) {
                    eof = true;
                    break;
                } else if (not res) {
                    out.append_range(tmp | adbfsm::sv::take(tmp_read));
                    co_return res.error();
                }
                tmp_read += *res;
            }
            out.append_range(tmp | adbfsm::sv::take(tmp_read));
        }

        co_return boost::system::error_code{};
    }

    adbfsm::Await<adbfsm::Expect<adbfsm::String>> exec_async(
        adbfsm::Str                     exe,
        adbfsm::Span<const adbfsm::Str> args,
        adbfsm::Str                     in,
        bool                            check
    )
    {
        adbfsm::log_d({ "{}: run {} {}" }, __func__, exe, args);

        namespace async = adbfsm::async;

        auto exec = co_await async::this_coro::executor;

        auto pipe_in  = async::pipe::Write{ exec };
        auto pipe_out = async::pipe::Read{ exec };
        auto pipe_err = async::pipe::Read{ exec };

        auto proc = bp::process{
            exec,
            bp::environment::find_executable(exe),
            args | adbfsm::sv::transform([](auto s) { return boost::string_view{ s.data(), s.size() }; }),
            bp::process_stdio{ pipe_in, pipe_out, pipe_err },
        };

        if (auto [ec, _] = co_await async::write_exact(pipe_in, in); ec) {
            adbfsm::log_e({ "{}: failed to read to stdin: {}" }, __func__, ec.message());
            co_return adbfsm::Unexpect{ async::to_generic_err(ec) };
        }
        pipe_in.close();

        auto out = std::string{};
        if (auto ec = co_await drain_pipe(pipe_out, out); ec and ec != boost::asio::error::eof) {
            adbfsm::log_e({ "{}: failed to read from stdout: {}" }, __func__, ec.message());
            co_return adbfsm::Unexpect{ async::to_generic_err(ec) };
        }

        adbfsm::log_d({ "{}: drained stdout: {:?}" }, __func__, out);

        auto err = std::string{};
        if (auto ec = co_await drain_pipe(pipe_err, err); ec and ec != boost::asio::error::eof) {
            adbfsm::log_e({ "{}: failed to read from stderr: {}" }, __func__, ec.message());
            co_return adbfsm::Unexpect{ async::to_generic_err(ec) };
        }

        auto ret = co_await proc.async_wait();
        if (check and ret != 0) {
            auto errmsg = not err.empty() ? adbfsm::util::strip(err) : adbfsm::util::strip(out);
            adbfsm::log_w({ "non-zero command status ({}) {} {}: err: [{}]" }, ret, exe, args, errmsg);
            co_return adbfsm::Unexpect{ to_errc(parse_stderr(errmsg)) };
        }

        co_return std::move(out);
    }

    template <typename T>
        requires std::is_fundamental_v<T>
    constexpr adbfsm::Opt<T> parse_fundamental(adbfsm::Str str, int base)
    {
        auto t         = T{};
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), t, base);
        if (ptr != str.data() + str.size() or ec != adbfsm::Errc{}) {
            return {};
        }
        return t;
    }

    adbfsm::Str get_basename(adbfsm::Str path)
    {
        if (path != "/") {
            auto found = adbfsm::sr::find(path | adbfsm::sv::reverse, '/');
            if (found != path.rend()) {    // no '/' means already basename
                return adbfsm::Str{ found.base(), path.end() };
            }
        }
        return path;
    }

    /**
     * @brief Parse the output of `stat -c '%f %h %s %u %g %X %Y %Z %n' <path>`
     */
    adbfsm::Opt<adbfsm::data::ParsedStat> parse_file_stat(adbfsm::Str str)
    {
        return adbfsm::util::split_n<8>(str, ' ').transform([](adbfsm::util::SplitResult<8>&& res) {
            auto [mode_hex, hardlinks, size, uid, gid, atime, mtime, ctime] = res.result;
            return adbfsm::data::ParsedStat{
                .stat = adbfsm::data::Stat{
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
    adbfsm::String resolve_path(adbfsm::path::Path parent, adbfsm::Str path)
    {
        auto parents = adbfsm::Vec<adbfsm::Str>{};
        if (path.front() != '/') {
            parents = adbfsm::util::split(parent.fullpath(), '/');
        }

        adbfsm::util::StringSplitter{ path, '/' }.while_next([&](adbfsm::Str str) {
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

        auto resolved = adbfsm::String{};
        for (auto path : parents) {
            resolved += '/';
            resolved += path;
        }

        return resolved;
    }

    // NOTE: somehow adb shell needs double escaping
    adbfsm::String quoted(adbfsm::path::Path path)
    {
        return fmt::format("\"{}\"", path.fullpath());
    }
}

namespace adbfsm::data
{
    using namespace std::string_view_literals;

    AExpect<Gen<ParsedStat>> Connection::statdir(path::Path path)
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

    AExpect<Stat> Connection::stat(path::Path path)
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

    AExpect<path::PathBuf> Connection::readlink(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "readlink"sv, Str{ qpath } };

        auto res = co_await exec_async("adb", args, "", true);

        co_return res.transform([&](String target) {
            auto target_path = resolve_path(path.parent_path(), util::strip(target));
            return path::create_buf(std::move(target_path)).value();
        });
    }

    AExpect<void> Connection::touch(path::Path path, bool create)
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

    AExpect<void> Connection::mkdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "mkdir"sv, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> Connection::rm(path::Path path, bool recursive)
    {
        const auto qpath = quoted(path);
        if (not recursive) {
            const auto args = Array{ "shell"sv, "rm"sv, Str{ qpath } };
            co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
        } else {
            const auto args = Array{ "shell"sv, "rm"sv, "-r"sv, Str{ qpath } };
            co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
        }
    }

    AExpect<void> Connection::rmdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto args  = Array{ "shell"sv, "rmdir"sv, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> Connection::mv(path::Path from, path::Path to)
    {
        const auto qfrom = quoted(from);
        const auto qto   = quoted(to);
        const auto args  = Array{ "shell"sv, "mv"sv, Str{ qfrom }, Str{ qto } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<void> Connection::truncate(path::Path path, off_t size)
    {
        const auto qpath = quoted(path);
        const auto sizes = fmt::format("{}", size);
        const auto args  = Array{ "shell"sv, "truncate"sv, "-s"sv, Str{ sizes }, Str{ qpath } };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<u64> Connection::open(path::Path /* path */, int /* flags */)
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

    AExpect<usize> Connection::read(path::Path path, Span<char> out, off_t offset)
    {
        const auto skip  = fmt::format("skip={}", offset);
        const auto count = fmt::format("count={}", out.size());
        const auto iff   = fmt::format("if=\"{}\"", path.fullpath());
        const auto bs    = fmt::format("bs={}", m_page_size);

        const auto args = Array{
            "shell"sv,
            "dd"sv,
            "iflag=skip_bytes,count_bytes"sv,    // https://stackoverflow.com/a/40792605/16506263
            Str{ bs },                           // https://superuser.com/a/234204
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

    AExpect<usize> Connection::write(path::Path path, Span<const char> in, off_t offset)
    {
        const auto seek = fmt::format("seek={}", offset);
        const auto off  = fmt::format("of=\"{}\"", path.fullpath());
        const auto bs   = fmt::format("bs={}", m_page_size);

        const auto args = Array{
            "shell"sv,
            "dd"sv,
            "oflag=seek_bytes"sv,    // same as above, though I don't know if this is really needed
            "conv=notrunc"sv,        // https://unix.stackexchange.com/a/146923
            Str{ bs },               // https://superuser.com/a/234204
            Str{ seek },
            Str{ off },
        };

        auto in_str = Str{ in.data(), in.size() };
        co_return (co_await exec_async("adb", args, in_str, true)).transform([&](auto&&) {
            return in_str.size();    // assume all the data is written to device
        });
    }

    AExpect<void> Connection::flush(path::Path /* path */)
    {
        /*
         * The streaming approach is immediate, so there is no need to flush the file. At least for the
         * moment. In the future we might want to implement caching once again.
         */

        return {};
    }

    AExpect<void> Connection::release(path::Path /* path */)
    {
        /*
         * The reason this part is a no-op is the same as the open function. We don't need to do any
         * open or close operation on the file.
         */

        return {};
    }

    AExpect<void> start_connection()
    {
        const auto args = Array{ "start-server"sv };
        co_return (co_await exec_async("adb", args, "", true)).transform(sink_void);
    }

    AExpect<Vec<Device>> list_devices()
    {
        const auto args = Array{ "devices"sv };
        auto       out  = co_await exec_async("adb", args, "", true);

        if (not out.has_value()) {
            co_return Unexpect{ out.error() };
        }

        auto devices = Vec<Device>{};

        auto line_splitter = util::StringSplitter{ *out, { '\n' } };
        std::ignore        = line_splitter.next();    // skip the first line

        while (auto str = line_splitter.next()) {
            auto splitter = util::StringSplitter{ *str, { " \t" } };

            auto serial_str = splitter.next();
            auto status_str = splitter.next();

            if (not serial_str.has_value() or not status_str.has_value()) {
                continue;
            }

            auto status = DeviceStatus::Unknown;

            // clang-format off
            if      (*status_str == "offline")      status = DeviceStatus::Offline;
            else if (*status_str == "unauthorized") status = DeviceStatus::Unauthorized;
            else if (*status_str == "device")       status = DeviceStatus::Device;
            // clang-format on

            devices.emplace_back(String{ *serial_str }, status);
        }

        co_return devices;
    }
}
