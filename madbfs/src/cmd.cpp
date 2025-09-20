#include "madbfs/cmd.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/split.hpp>

#define BOOST_PROCESS_VERSION 2
#include <boost/process.hpp>

namespace
{
    // adb error patterns.
    namespace error
    {
        static constexpr madbfs::Str no_device           = "adb: no devices/emulators found";
        static constexpr madbfs::Str device_offline      = "adb: device offline";
        static constexpr madbfs::Str permission_denied   = " Permission denied";
        static constexpr madbfs::Str no_such_file_or_dir = " No such file or directory";
        static constexpr madbfs::Str not_a_directory     = " Not a directory";
        static constexpr madbfs::Str inaccessible        = " inaccessible or not found";
        static constexpr madbfs::Str read_only           = " Read-only file system";
    }

    /**
     * @enum AdbError
     *
     * @brief Possible adb command errors.
     */
    enum class AdbError
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

    /**
     * @brief Get no device not found error string if `ANDROID_SERIAL` env variable is not defined.
     *
     * The string will be empty if `ANDROID_SERIAL` is defined.
     */
    inline madbfs::Str get_no_dev_serial()
    {
        if (auto* serial = std::getenv("ANDROID_SERIAL"); serial != nullptr) {
            static auto no_dev_serial = fmt::format("adb: device '{}' not found", serial);
            return no_dev_serial;
        }
        return {};
    }

    /**
     * @brief Convert adb errors to generic error conditions.
     *
     * @param err adb error.
     */
    inline madbfs::Errc to_errc(AdbError err)
    {
        using Err = AdbError;
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

    /**
     * @brief Parse output from stderr into `AdbError` enumeration.
     *
     * @param str Stderr output.
     *
     * @return AdbError.
     *
     * Will return `AdbError::Unknown` if the stderr output does not match any enumeration.
     */
    inline AdbError parse_stderr(madbfs::Str str)
    {
        using Err = AdbError;

        auto splitter = madbfs::util::StringSplitter{ str, '\n' };
        while (auto line = splitter.next()) {
            if (*line == error::no_device or *line == error::device_offline) {
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
            if      (eq(error::permission_denied))   return Err::PermDenied;
            else if (eq(error::no_such_file_or_dir)) return Err::NoSuchFileOrDir;
            else if (eq(error::not_a_directory))     return Err::NotADir;
            else if (eq(error::inaccessible))        return Err::Inaccessible;
            else if (eq(error::read_only))           return Err::ReadOnly;
            else                              return Err::Unknown;
            // clang-format on
        }

        return Err::Unknown;
    }

    /**
     * @brief Drain pipe into buffer.
     *
     * @param rpipe Readable pipe.
     * @param out Output buffer.
     *
     * @return An error code. If the returned error is `EOF`, then draining is successful.
     */
    madbfs::Await<boost::system::error_code> drain_pipe(madbfs::async::pipe::Read& rpipe, madbfs::String& out)
    {
        out.clear();

        auto tmp = madbfs::Array<char, 1024>{};
        auto eof = false;

        while (not eof) {
            auto tmp_read = 0uz;
            while (tmp_read < tmp.size()) {
                auto buf = madbfs::net::buffer(tmp.data() + tmp_read, tmp.size() - tmp_read);
                auto res = co_await rpipe.async_read_some(buf);
                if (not res and res == madbfs::net::error::eof) {
                    eof = true;
                    break;
                } else if (not res) {
                    auto rest = tmp | madbfs::sv::take(tmp_read);
                    out.insert(out.end(), rest.begin(), rest.end());
                    co_return res.error();
                }
                tmp_read += *res;
            }
            auto rest = tmp | madbfs::sv::take(tmp_read);
            out.insert(out.end(), rest.begin(), rest.end());
        }

        co_return boost::system::error_code{};
    }
}

namespace madbfs::cmd
{
    AExpect<String> exec(Span<const Str> cmd, Str in, bool check, bool merge_err)
    {
        assert(not cmd.empty());

        namespace bp = boost::process::v2;

        log_d("{}: run {}", __func__, cmd);

        auto exec = co_await async::current_executor();

        auto pipe_in  = async::pipe::Write{ exec };
        auto pipe_out = async::pipe::Read{ exec };
        auto pipe_err = async::pipe::Read{ exec };

        auto to_boost_str = [](auto s) { return boost::string_view{ s.data(), s.size() }; };

        auto exe  = bp::environment::find_executable(cmd[0]);
        auto args = cmd | sv::drop(1) | sv::transform(to_boost_str);

        auto proc = bp::process{ exec, exe, args, bp::process_stdio{ pipe_in, pipe_out, pipe_err } };
        auto ec   = boost::system::error_code{};

        // NOTE: synchronous write to prevent interleaving
        if (auto n = net::write(pipe_in, net::buffer(in), ec); ec) {
            log_e("{}: failed to write to stdin: {}", __func__, ec.message());
            co_return Unexpect{ async::to_generic_err(ec) };
        } else if (n < in.size()) {
            co_return Unexpect{ Errc::broken_pipe };
        }
        pipe_in.close();

        auto out = String{};
        if (auto ec = co_await drain_pipe(pipe_out, out); ec and ec != net::error::eof) {
            log_e("{}: failed to read from stdout: {}", __func__, ec.message());
            co_return Unexpect{ async::to_generic_err(ec) };
        }

        auto err = String{};
        if (auto ec = co_await drain_pipe(pipe_err, err); ec and ec != net::error::eof) {
            log_e("{}: failed to read from stderr: {}", __func__, ec.message());
            co_return Unexpect{ async::to_generic_err(ec) };
        }

        auto ret = co_await proc.async_wait();
        if (check and ret != 0) {
            auto errmsg = not err.empty() ? util::strip(err) : util::strip(out);
            log_i("non-zero command status ({}) {}: err: [{}]", ret, cmd, errmsg);
            co_return Unexpect{ to_errc(parse_stderr(errmsg)) };
        }

        if (merge_err) {
            out += err;
        }

        co_return std::move(out);
    }
}
