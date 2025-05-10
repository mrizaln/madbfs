#include "adbfsm/log.hpp"
#include "adbfsm/util/split.hpp"

#include "adbfsm/data/connection.hpp"
#include "adbfsm/path/path.hpp"

#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>

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

    adbfsm::Expect<adbfsm::String> exec(adbfsm::Span<const adbfsm::Str> cmd)
    {
        // adbfsm::log_d({ "exec command: {::?}" }, cmd);    // quite heavy to log :D

        using Sink = reproc::sink::string;
        using namespace std::chrono_literals;

        auto proc = reproc::process{};
        auto args = reproc::arguments{ cmd };
        auto opts = reproc::options{};

        opts.redirect.err.type = reproc::redirect::pipe;

        auto out = adbfsm::String{};
        auto err = adbfsm::String{};

        auto ec             = proc.start(args, opts);
        auto ec_drain       = reproc::drain(proc, Sink{ out }, Sink{ err });
        auto [ret, ec_wait] = proc.wait(10s);

        if (ec) {
            adbfsm::log_e({ "failed to start command {}: {}" }, cmd, ec.message());
            return adbfsm::Unexpect{ adbfsm::Errc::protocol_error };
        }
        if (ec_wait) {
            adbfsm::log_e({ "failed to execute command {}: {}" }, cmd, ec_wait.message());
            return adbfsm::Unexpect{ adbfsm::Errc::timed_out };
        }
        if (ec_drain) {
            adbfsm::log_e({ "failed to drain command output {}: {}" }, cmd, ec_drain.message());
            return adbfsm::Unexpect{ static_cast<adbfsm::Errc>(ec_drain.value()) };
        }

        if (ret != 0) {
            auto errmsg = not err.empty() ? adbfsm::util::strip(err) : adbfsm::util::strip(out);
            adbfsm::log_w({ "non-zero command status ({}) {}: err: [{}]" }, ret, cmd, errmsg);
            return adbfsm::Unexpect{ to_errc(parse_stderr(errmsg)) };
        }
        return std::move(out);
    }

    /**
     * @brief Execute a command and call the output function with the output.
     *
     * @param cmd The command to execute.
     * @param in The input to the command (stdin).
     * @param outfn Output function to call with the output.
     *
     * @return The number of bytes written to the command's stdin.
     */
    template <std::invocable<adbfsm::Span<const adbfsm::u8>> Out>
    adbfsm::Expect<adbfsm::usize> exec_fn(
        adbfsm::Span<const adbfsm::Str> cmd,
        adbfsm::Span<const char>        in,
        Out                             outfn
    )
    {
        // adbfsm::log_d({ "exec command: {::?}" }, cmd);    // quite heavy to log :D

        using namespace std::chrono_literals;

        auto proc = reproc::process{};
        auto args = reproc::arguments{ cmd };
        auto opts = reproc::options{};

        opts.redirect.err.type = reproc::redirect::pipe;
        if (not in.empty()) {
            opts.redirect.in.type = reproc::redirect::pipe;
        }

        auto ec       = proc.start(args, opts);
        auto write_in = 0uz;

        if (not in.empty()) {
            auto [write, ec_in] = proc.write(reinterpret_cast<const adbfsm::u8*>(in.data()), in.size());
            if (ec_in) {
                adbfsm::log_e({ "failed to write command input {}: {}" }, cmd, ec_in.message());
                return adbfsm::Unexpect{ adbfsm::Errc::protocol_error };
            }
            if (ec_in = proc.close(reproc::stream::in); ec_in) {
                adbfsm::log_e({ "failed to close command input {}: {}" }, cmd, ec_in.message());
                return adbfsm::Unexpect{ adbfsm::Errc::protocol_error };
            }
            write_in = write;
        }

        auto err = adbfsm::String{};
        auto out = [&](reproc::stream, const adbfsm::u8* buffer, adbfsm::usize size) {
            outfn(adbfsm::Span{ buffer, size });
            return std::error_code{};
        };

        auto ec_drain       = reproc::drain(proc, out, reproc::sink::string{ err });
        auto [ret, ec_wait] = proc.wait(10s);

        if (ec) {
            adbfsm::log_e({ "failed to start command {}: {}" }, cmd, ec.message());
            return adbfsm::Unexpect{ adbfsm::Errc::protocol_error };
        }
        if (ec_wait) {
            adbfsm::log_e({ "failed to execute command {}: {}" }, cmd, ec_wait.message());
            return adbfsm::Unexpect{ adbfsm::Errc::timed_out };
        }
        if (ec_drain) {
            adbfsm::log_e({ "failed to drain command output {}: {}" }, cmd, ec_drain.message());
            return adbfsm::Unexpect{ static_cast<adbfsm::Errc>(ec_drain.value()) };
        }

        if (ret != 0) {
            auto errmsg = adbfsm::util::strip(err);
            adbfsm::log_w({ "non-zero command status ({}) {}: err: [{}]" }, ret, cmd, errmsg);
            return adbfsm::Unexpect{ to_errc(parse_stderr(err)) };
        }

        return write_in;
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

    Expect<Gen<ParsedStat>> Connection::statdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto cmd   = Array{
            "adb"sv, "shell"sv, "find"sv, Str{ qpath }, "-maxdepth"sv,
            "1"sv,   "-exec"sv, "stat"sv, "-c"sv,       "'%f %h %s %u %g %X %Y %Z %n'"sv,
            "{}"sv,  "+"sv,
        };

        auto out = exec(cmd);
        if (not out.has_value()) {
            return Unexpect{ out.error() };
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

        return generator(std::move(*out));
    }

    Expect<Stat> Connection::stat(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto cmd   = Array{
            "adb"sv, "shell"sv, "stat"sv, "-c"sv, "'%f %h %s %u %g %X %Y %Z %n'"sv, Str{ qpath },
        };

        return exec(cmd).and_then([&](String out) {
            return ok_or(parse_file_stat(util::strip(out)), Errc::io_error)
                .transform([](ParsedStat parsed) { return parsed.stat; })
                .transform_error([&](auto err) {
                    log_e({ "Connection::stat: parsing stat failed [{}]" }, path.fullpath());
                    return err;
                });
        });
    }

    Expect<path::PathBuf> Connection::readlink(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto cmd   = Array{ "adb"sv, "shell"sv, "readlink"sv, Str{ qpath } };
        return exec(cmd).transform([&](String target) {
            auto target_path = resolve_path(path.parent_path(), util::strip(target));
            return path::create_buf(std::move(target_path)).value();
        });
    }

    Expect<void> Connection::touch(path::Path path, bool create)
    {
        const auto qpath = quoted(path);
        if (create) {
            const auto cmd = Array{ "adb"sv, "shell"sv, "touch"sv, Str{ qpath } };
            return exec(cmd).transform(sink_void);
        } else {
            const auto cmd = Array{ "adb"sv, "shell"sv, "touch"sv, "-c"sv, Str{ qpath } };
            return exec(cmd).transform(sink_void);
        }
    }

    Expect<void> Connection::mkdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto cmd   = Array{ "adb"sv, "shell"sv, "mkdir"sv, Str{ qpath } };
        return exec(cmd).transform(sink_void);
    }

    Expect<void> Connection::rm(path::Path path, bool recursive)
    {
        const auto qpath = quoted(path);
        if (not recursive) {
            const auto cmd = Array{ "adb"sv, "shell"sv, "rm"sv, Str{ qpath } };
            return exec(cmd).transform(sink_void);
        } else {
            const auto cmd = Array{ "adb"sv, "shell"sv, "rm"sv, "-r"sv, Str{ qpath } };
            return exec(cmd).transform(sink_void);
        }
    }

    Expect<void> Connection::rmdir(path::Path path)
    {
        const auto qpath = quoted(path);
        const auto cmd   = Array{ "adb"sv, "shell"sv, "rmdir"sv, Str{ qpath } };
        return exec(cmd).transform(sink_void);
    }

    Expect<void> Connection::mv(path::Path from, path::Path to)
    {
        const auto qfrom = quoted(from);
        const auto qto   = quoted(to);
        const auto cmd   = Array{ "adb"sv, "shell"sv, "mv"sv, Str{ qfrom }, Str{ qto } };
        return exec(cmd).transform(sink_void);
    }

    Expect<void> Connection::truncate(path::Path path, off_t size)
    {
        const auto qpath = quoted(path);
        const auto sizes = fmt::format("{}", size);
        const auto cmd   = Array{ "adb"sv, "shell"sv, "truncate"sv, "-s"sv, Str{ sizes }, Str{ qpath } };
        return exec(cmd).transform(sink_void);
    }

    Expect<u64> Connection::open(path::Path /* path */, int /* flags */)
    {
        /*
         * Since we're using a streaming approach to read/write files, there is no need to do open or
         * close operation on the file. The file is opened when we read or write to it, and closed after
         * those operation complete on the device.
         *
         * Thus, there is really no need to do anything here.
         */

        return m_counter.fetch_add(1, std::memory_order::relaxed) + 1;
    }

    Expect<usize> Connection::read(path::Path path, Span<char> out, off_t offset)
    {
        const auto skip  = fmt::format("skip={}", offset);
        const auto count = fmt::format("count={}", out.size());
        const auto iff   = fmt::format("if=\"{}\"", path.fullpath());
        const auto bs    = fmt::format("bs={}", m_page_size);

        const auto cmd = Array{
            "adb"sv,
            "shell"sv,
            "dd"sv,
            "iflag=skip_bytes,count_bytes"sv,    // https://stackoverflow.com/a/40792605/16506263
            Str{ bs },                           // https://superuser.com/a/234204
            Str{ skip },
            Str{ count },
            Str{ iff },
        };

        auto read_count = 0uz;
        auto outfn      = [&](Span<const u8> data) {
            auto remaining = out.size() - read_count;
            auto to_copy   = std::min(data.size(), remaining);

            std::copy_n(data.data(), to_copy, out.data() + read_count);
            read_count += to_copy;
        };

        return exec_fn(cmd, "", outfn).transform([&](usize) { return read_count; });
    }

    Expect<usize> Connection::write(path::Path path, Span<const char> in, off_t offset)
    {
        const auto seek = fmt::format("seek={}", offset);
        const auto off  = fmt::format("of=\"{}\"", path.fullpath());
        const auto bs   = fmt::format("bs={}", m_page_size);

        const auto cmd = Array{
            "adb"sv,
            "shell"sv,
            "dd"sv,
            "oflag=seek_bytes"sv,    // same as above, though I don't know if this is really needed
            "conv=notrunc"sv,        // https://unix.stackexchange.com/a/146923
            Str{ bs },               // https://superuser.com/a/234204
            Str{ seek },
            Str{ off },
        };

        return exec_fn(cmd, in, [&](Span<const u8>) { /* ignore */ });
    }

    Expect<void> Connection::flush(path::Path /* path */)
    {
        /*
         * The streaming approach is immediate, so there is no need to flush the file. At least for the
         * moment. In the future we might want to implement caching once again.
         */

        return {};
    }

    Expect<void> Connection::release(path::Path /* path */)
    {
        /*
         * The reason this part is a no-op is the same as the open function. We don't need to do any
         * open or close operation on the file.
         */

        return {};
    }

    Expect<void> start_connection()
    {
        const auto cmd = Array{ "adb"sv, "start-server"sv };
        return exec(cmd).transform(sink_void);
    }

    Expect<Vec<Device>> list_devices()
    {
        const auto cmd = Array{ "adb"sv, "devices"sv };
        auto       out = exec(cmd);

        if (not out.has_value()) {
            return Unexpect{ out.error() };
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

        return devices;
    }
}
