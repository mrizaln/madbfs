#include "madbfs/connection/connection.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/split.hpp>

#define BOOST_PROCESS_VERSION 2
#include <boost/process.hpp>

namespace
{
    static constexpr madbfs::Str no_device           = "adb: no devices/emulators found";
    static constexpr madbfs::Str device_offline      = "adb: device offline";
    static constexpr madbfs::Str permission_denied   = " Permission denied";
    static constexpr madbfs::Str no_such_file_or_dir = " No such file or directory";
    static constexpr madbfs::Str not_a_directory     = " Not a directory";
    static constexpr madbfs::Str inaccessible        = " inaccessible or not found";
    static constexpr madbfs::Str read_only           = " Read-only file system";

    inline madbfs::Str get_no_dev_serial()
    {
        if (auto* serial = std::getenv("ANDROID_SERIAL"); serial != nullptr) {
            static auto no_dev_serial = fmt::format("adb: device '{}' not found", serial);
            return no_dev_serial;
        }
        return {};
    }

    inline madbfs::Errc to_errc(madbfs::connection::AdbError err)
    {
        using Err = madbfs::connection::AdbError;
        switch (err) {
        case Err::Unknown: return madbfs::Errc::io_error;
        case Err::NoDev: return madbfs::Errc::no_such_device;
        case Err::PermDenied: return madbfs::Errc::permission_denied;
        case Err::NoSuchFileOrDir: return madbfs::Errc::no_such_file_or_directory;
        case Err::NotADir: return madbfs::Errc::not_a_directory;
        case Err::Inaccessible: return madbfs::Errc::operation_not_supported;
        case Err::ReadOnly: return madbfs::Errc::read_only_file_system;
        case Err::TryAgain: return madbfs::Errc::resource_unavailable_try_again;
        default: std::terminate();
        }
    }

    inline madbfs::connection::AdbError parse_stderr(madbfs::Str str)
    {
        using Err = madbfs::connection::AdbError;

        auto splitter = madbfs::util::StringSplitter{ str, '\n' };
        while (auto line = splitter.next()) {
            if (*line == no_device or *line == device_offline) {
                return Err::NoDev;
            } else if (*line == get_no_dev_serial()) {
                return Err::TryAgain;
            }

            auto rev       = madbfs::String{ line->rbegin(), line->rend() };
            auto rev_strip = madbfs::util::strip(rev);
            auto err       = madbfs::util::StringSplitter{ rev_strip, ':' }.next();
            if (not err) {
                continue;
            }

            auto eq = [&](auto rhs) { return madbfs::sr::equal(*err, rhs | madbfs::sv::reverse); };

            // clang-format off
            if      (eq(permission_denied))   return Err::PermDenied;
            else if (eq(no_such_file_or_dir)) return Err::NoSuchFileOrDir;
            else if (eq(not_a_directory))     return Err::NotADir;
            else if (eq(inaccessible))        return Err::Inaccessible;
            else if (eq(read_only))           return Err::ReadOnly;
            else                              return Err::Unknown;
            // clang-format on
        }

        return Err::Unknown;
    }

    inline madbfs::Await<boost::system::error_code> drain_pipe(
        madbfs::async::pipe::Read& rpipe,
        madbfs::String&            out
    )
    {
        out.clear();

        auto tmp = std::array<char, 1024>{};
        auto eof = false;

        while (not eof) {
            auto tmp_read = 0uz;
            while (tmp_read < tmp.size()) {
                auto buf = madbfs::async::buffer(tmp.data() + tmp_read, tmp.size() - tmp_read);
                auto res = co_await rpipe.async_read_some(buf);
                if (not res and res == madbfs::asio::error::eof) {
                    eof = true;
                    break;
                } else if (not res) {
                    out.append_range(tmp | madbfs::sv::take(tmp_read));
                    co_return res.error();
                }
                tmp_read += *res;
            }
            out.append_range(tmp | madbfs::sv::take(tmp_read));
        }

        co_return boost::system::error_code{};
    }
}

namespace madbfs::connection
{
    using namespace std::string_view_literals;

    Str to_string(DeviceStatus status)
    {
        switch (status) {
        case DeviceStatus::Device: return "device ok";
        case DeviceStatus::Offline: return "device offline";
        case DeviceStatus::Unauthorized: return "device unauthorized";
        case DeviceStatus::Unknown: return "unknown";
        }
        return "Unknown";
    }

    madbfs::Await<madbfs::Expect<madbfs::String>> exec_async(
        madbfs::Str                     exe,
        madbfs::Span<const madbfs::Str> args,
        madbfs::Str                     in,
        bool                            check,
        bool                            merge_err
    )
    {
        namespace bp    = boost::process::v2;
        namespace async = madbfs::async;

        madbfs::log_d("{}: run {} {}", __func__, exe, args);

        auto exec = co_await async::current_executor();

        auto pipe_in  = async::pipe::Write{ exec };
        auto pipe_out = async::pipe::Read{ exec };
        auto pipe_err = async::pipe::Read{ exec };

        auto proc = bp::process{
            exec,
            bp::environment::find_executable(exe),
            args | madbfs::sv::transform([](auto s) { return boost::string_view{ s.data(), s.size() }; }),
            bp::process_stdio{ pipe_in, pipe_out, pipe_err },
        };

        // NOTE: if I write to stdin asynchronously, for some reason the write will be corrupted. at least
        // that is what happen on multiple dd command to the same file with disjoint offset with fixed size.

        // if (auto n = co_await async::write_exact(pipe_in, in); not n) {
        //     madbfs::log_e("{}: failed to write to stdin: {}", __func__, n.error().message());
        //     co_return madbfs::Unexpect{ async::to_generic_err(n.error()) };
        // }
        // pipe_in.close();

        // NOTE: [cont.] switching to writing the data at once with blocking operation fixed the issue.
        // TODO: make the async code correct

        auto ec = boost::system::error_code{};
        if (auto n = asio::write(pipe_in, async::buffer(in), ec); ec) {
            madbfs::log_e("{}: failed to write to stdin: {}", __func__, ec.message());
            co_return madbfs::Unexpect{ async::to_generic_err(ec) };
        } else if (n < in.size()) {
            co_return madbfs::Unexpect{ madbfs::Errc::broken_pipe };
        }
        pipe_in.close();

        auto out = std::string{};
        if (auto ec = co_await drain_pipe(pipe_out, out); ec and ec != asio::error::eof) {
            madbfs::log_e("{}: failed to read from stdout: {}", __func__, ec.message());
            co_return madbfs::Unexpect{ async::to_generic_err(ec) };
        }

        auto err = std::string{};
        if (auto ec = co_await drain_pipe(pipe_err, err); ec and ec != asio::error::eof) {
            madbfs::log_e("{}: failed to read from stderr: {}", __func__, ec.message());
            co_return madbfs::Unexpect{ async::to_generic_err(ec) };
        }

        auto ret = co_await proc.async_wait();
        if (check and ret != 0) {
            auto errmsg = not err.empty() ? madbfs::util::strip(err) : madbfs::util::strip(out);
            madbfs::log_w("non-zero command status ({}) {} {}: err: [{}]", ret, exe, args, errmsg);
            co_return madbfs::Unexpect{ to_errc(parse_stderr(errmsg)) };
        }

        if (merge_err) {
            out += err;
        }

        co_return std::move(out);
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
