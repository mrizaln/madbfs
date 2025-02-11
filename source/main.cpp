#include "adbfsm.hpp"
#include "cmd.hpp"
#include "util.hpp"

#include <fmt/base.h>

#include <execinfo.h>
#include <fuse_opt.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <array>

enum class SerialStatus
{
    NotExist,
    Offline,
    Unauthorized,
    Device,
};

std::string_view to_string(SerialStatus status)
{
    switch (status) {
    case SerialStatus::NotExist: return "not exist";
    case SerialStatus::Offline: return "offline";
    case SerialStatus::Unauthorized: return "unauthorized";
    case SerialStatus::Device: return "device";
    }
    return "Unknown";
}

/**
 * @brief Create a temporary directory.
 * @return The path to the temporary directory.
 *
 * The created directory handling, including its removal, is up to the caller.
 */
std::filesystem::path make_temp_dir()
{
    char adbfsm_template[] = "/tmp/adbfsm-XXXXXX";
    auto temp              = mkdtemp(adbfsm_template);
    adbfsm::log_i({ "created temporary directory: {:?}" }, temp);
    return temp;
}

/**
 * @brief SIGSEGV handler to print stack trace.
 *
 * This handler is based on Stack Overflow answer:
 * - https://stackoverflow.com/a/77336/16506263
 */
void sigsegv_handler(int)
{
    auto array = std::array<void*, 10>{};
    auto size  = static_cast<int>(array.size());

    // get void*'s for all entries on the stack
    size = backtrace(array.data(), array.size());

    // print out all the frames to stderr
    fmt::print(stderr, "SEGFAULT signal raised, backtrace:\n");
    backtrace_symbols_fd(array.data(), size, STDERR_FILENO);

    // remove the temporary directory
    if (auto* fuse_ctx = fuse_get_context(); fuse_ctx != nullptr) {
        auto& data = *static_cast<adbfsm::AdbfsmData*>(fuse_ctx->private_data);
        std::filesystem::remove_all(data.m_dir);
    }

    // re-raise the signal to get the default handler to run
    std::signal(SIGSEGV, SIG_DFL);
    std::raise(SIGSEGV);
}

void terminate_handler()
{
    auto exception = std::current_exception();
    if (exception != nullptr) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            fmt::print(stderr, "Termination reached, uncaught exception: {}\n", e.what());
        } catch (...) {
            fmt::print(stderr, "Termination reached, uncaught exception\n");
        }
    }

    auto array = std::array<void*, 10>{};
    auto size  = static_cast<int>(array.size());

    // get void*'s for all entries on the stack
    size = backtrace(array.data(), array.size());

    // print out all the frames to stderr
    fmt::print(stderr, "backtrace:\n");
    backtrace_symbols_fd(array.data(), size, STDERR_FILENO);

    // remove the temporary directory
    if (auto* fuse_ctx = fuse_get_context(); fuse_ctx != nullptr) {
        auto& data = *static_cast<adbfsm::AdbfsmData*>(fuse_ctx->private_data);
        std::filesystem::remove_all(data.m_dir);
    }
}

void show_help(const char* prog, bool err)
{
    auto out = err ? stderr : stdout;
    fmt::print(out, "usage: {} [options] <mountpoint>\n\n", prog);
    fmt::print(
        out,
        "Adbfsm specific options:\n"
        "    --serial=<s>        The serial number of the device to mount (REQUIRED)\n"
        "    --cachesize=<n>     The maximum size of the cache in MB (default: 500)\n"
        "    --rescan            Perform rescan every (idk)\n"
        "    --help              Show this help message\n"
        "    --fuse-help         Show full help message\n\n"
    );
}

SerialStatus check_serial(std::string_view serial)
{
    auto proc = adbfsm::cmd::exec({ "adb", "devices" });
    assert(proc.returncode == 0);

    auto line_splitter = adbfsm::util::StringSplitter{ proc.cout, { '\n' } };
    while (auto str = line_splitter.next()) {
        auto splitter = adbfsm::util::StringSplitter{ *str, { " \t" } };

        auto supposedly_serial = splitter.next();
        auto status            = splitter.next();

        if (not supposedly_serial.has_value() or not status.has_value()) {
            return SerialStatus::NotExist;
        }

        if (supposedly_serial.value() == serial) {
            if (*status == "offline") {
                return SerialStatus::Offline;
            } else if (*status == "unauthorized") {
                return SerialStatus::Unauthorized;
            } else if (*status == "device") {
                return SerialStatus::Device;
            } else {
                return SerialStatus::NotExist;
            }
        }
    }

    return SerialStatus::NotExist;
}

int main(int argc, char** argv)
{
    adbfsm::log::init(spdlog::level::debug);
    adbfsm::log_i({ "starting adb server..." });

    if (auto serv = adbfsm::cmd::exec({ "adb", "start-server" }); serv.returncode != 0) {
        fmt::println(stderr, "error: failed to start adb server, make sure adb is installed and in PATH");
        fmt::println(stderr, "stderr:\n{}", serv.cerr);
        return 1;
    }

    std::signal(SIGSEGV, sigsegv_handler);
    std::set_terminate(terminate_handler);

    auto adbfsm_oper = fuse_operations{
        .getattr     = adbfsm::getattr,
        .readlink    = adbfsm::readlink,
        .getdir      = nullptr,
        .mknod       = adbfsm::mknod,
        .mkdir       = adbfsm::mkdir,
        .unlink      = adbfsm::unlink,
        .rmdir       = adbfsm::rmdir,
        .symlink     = nullptr,
        .rename      = adbfsm::rename,
        .link        = nullptr,
        .chmod       = nullptr,
        .chown       = nullptr,
        .truncate    = adbfsm::truncate,
        .utime       = nullptr,
        .open        = adbfsm::open,
        .read        = adbfsm::read,
        .write       = adbfsm::write,
        .statfs      = nullptr,
        .flush       = adbfsm::flush,
        .release     = adbfsm::release,
        .fsync       = nullptr,
        .setxattr    = nullptr,
        .getxattr    = nullptr,
        .listxattr   = nullptr,
        .removexattr = nullptr,
        .opendir     = nullptr,
        .readdir     = adbfsm::readdir,
        .releasedir  = nullptr,
        .fsyncdir    = nullptr,
        .init        = nullptr,
        .destroy     = adbfsm::destroy,
        .access      = adbfsm::access,
        .create      = nullptr,
        .ftruncate   = nullptr,
        .fgetattr    = nullptr,
        .lock        = nullptr,
        .utimens     = adbfsm::utimens,
        .bmap        = nullptr,

        .flag_nullpath_ok   = {},
        .flag_nopath        = {},
        .flag_utime_omit_ok = {},
        .flag_reserved      = {},

        .ioctl     = nullptr,
        .poll      = nullptr,
        .write_buf = nullptr,
        .read_buf  = nullptr,
        .flock     = nullptr,
        .fallocate = nullptr,
    };

    // don't set default value here
    struct AdbfsmOpt
    {
        const char* m_serial    = nullptr;
        int         m_cachesize = 500;
        int         m_rescan    = false;
        int         m_help      = false;
        int         m_fuse_help = false;
    };

    fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt adbfsm_opt_spec[] = {
        { "--serial=%s", offsetof(AdbfsmOpt, m_serial), true },
        { "--cachesize=%d", offsetof(AdbfsmOpt, m_cachesize), true },
        { "--rescan", offsetof(AdbfsmOpt, m_rescan), true },
        { "-h", offsetof(AdbfsmOpt, m_help), true },
        { "--help", offsetof(AdbfsmOpt, m_help), true },
        { "--fuse-help", offsetof(AdbfsmOpt, m_fuse_help), true },
        FUSE_OPT_END,
    };

    auto adbfsm_opt = AdbfsmOpt{};
    if (fuse_opt_parse(&args, &adbfsm_opt, adbfsm_opt_spec, NULL) != 0) {
        fmt::println(stderr, "error: failed to parse options\n");
        fmt::println(stderr, "try '{} --help' for more information", argv[0]);
        fmt::println(stderr, "try '{} --fuse-help' for full information", argv[0]);
        return 1;
    }

    if (adbfsm_opt.m_help) {
        show_help(argv[0], false);
        fuse_opt_free_args(&args);
        return 0;
    } else if (adbfsm_opt.m_serial == nullptr and not adbfsm_opt.m_fuse_help) {
        fmt::println(stderr, "error: --serial is required");
        fmt::println(stderr, "run 'adb devices' command to get the desired device serial number\n");
        show_help(argv[0], true);
        fuse_opt_free_args(&args);
        return 1;
    }

    if (adbfsm_opt.m_fuse_help) {
        show_help(argv[0], false);
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';

        auto ret = fuse_main(args.argc, args.argv, &adbfsm_oper, NULL);
        fuse_opt_free_args(&args);

        return ret;
    }

    if (auto status = check_serial(adbfsm_opt.m_serial); status != SerialStatus::Device) {
        fmt::println(stderr, "error: serial '{} 'is not valid ({})", adbfsm_opt.m_serial, to_string(status));
        fuse_opt_free_args(&args);
        return 1;
    }

    auto cache_mb = static_cast<std::size_t>(std::max(adbfsm_opt.m_cachesize, 500));

    auto data = adbfsm::AdbfsmData{
        .m_cache      = {},
        .m_local_copy = { cache_mb * 1000 * 1000 },
        .m_dir        = make_temp_dir(),
        .m_serial     = adbfsm_opt.m_serial,
        .m_readdir    = false,
        .m_rescan     = static_cast<bool>(adbfsm_opt.m_rescan),
    };

    adbfsm::log_i({ "mounting device with serial '{}' and cache size {} MB" }, data.m_serial, cache_mb);

    auto ret = fuse_main(args.argc, args.argv, &adbfsm_oper, (void*)&data);
    fuse_opt_free_args(&args);

    return ret;
}
