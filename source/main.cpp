#include "adbfsm.hpp"
#include "cmd.hpp"
#include "util.hpp"

#include <fmt/base.h>

#include <execinfo.h>
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

    // print out all the frames to logger as well
    adbfsm::log_c({ "backtrace:" });
    auto names = backtrace_symbols(array.data(), size);
    for (auto i : adbfsm::sv::iota(0, size)) {
        adbfsm::log_c({ "{}" }, names[i]);
    }
    free(names);

    // remove the temporary directory
    if (auto* fuse_ctx = fuse_get_context(); fuse_ctx != nullptr) {
        adbfsm::log_c({ "there is no fuse context! temp dir can't be deleted" });
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

    // print out all the frames to logger as well
    adbfsm::log_c({ "backtrace:" });
    auto names = backtrace_symbols(array.data(), size);
    for (auto i : adbfsm::sv::iota(0, size)) {
        adbfsm::log_c({ "{}" }, names[i]);
    }
    free(names);

    // remove the temporary directory
    if (auto* fuse_ctx = fuse_get_context(); fuse_ctx != nullptr) {
        adbfsm::log_c({ "there is no fuse context! temp dir can't be deleted" });
        auto& data = *static_cast<adbfsm::AdbfsmData*>(fuse_ctx->private_data);
        std::filesystem::remove_all(data.m_dir);
    }
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

std::string get_serial()
{
    auto proc = adbfsm::cmd::exec({ "adb", "devices" });
    assert(proc.returncode == 0);

    auto serials       = std::vector<std::string>{};
    auto line_splitter = adbfsm::util::StringSplitter{ proc.cout, { '\n' } };

    while (auto str = line_splitter.next()) {
        auto splitter = adbfsm::util::StringSplitter{ *str, { " \t" } };

        auto supposedly_serial = splitter.next();
        auto status            = splitter.next();

        if (not supposedly_serial.has_value() or not status.has_value()) {
            break;
        }
        if (*status == "device") {
            serials.emplace_back(*supposedly_serial);
        }
    }

    if (serials.empty()) {
        return {};
    } else if (serials.size() == 1) {
        fmt::println("[adbfsm] only one device found, using serial '{}'", serials[0]);
        return serials[0];
    }

    fmt::println("[adbfsm] multiple devices detected, please specify which one you would like to use:");
    for (auto i : adbfsm::sv::iota(0u, serials.size())) {
        fmt::println("         - {}: {}", i + 1, serials[i]);
    }
    fmt::print("\n[adbfsm] enter the number of the device you would like to use: ");

    auto choice = 1u;
    while (true) {
        std::cin >> choice;
        if (choice > 0 and choice <= serials.size()) {
            break;
        }
        fmt::print("[adbfsm] invalid choice, please enter a number between 1 and {}: ", serials.size());
    }
    fmt::println("[adbfsm] using serial '{}'", serials[choice - 1]);

    return serials[choice - 1];
}

std::optional<spdlog::level::level_enum> parse_level_str(std::string_view level)
{
    // clang-format off
    if      (level == "trace")      return spdlog::level::trace;
    else if (level == "debug")      return spdlog::level::debug;
    else if (level == "info")       return spdlog::level::info;
    else if (level == "warn")       return spdlog::level::warn;
    else if (level == "error")      return spdlog::level::err;
    else if (level == "critical")   return spdlog::level::critical;
    else if (level == "off")        return spdlog::level::off;
    else                            return std::nullopt;
    // clang-format on
}

int main(int argc, char** argv)
{
    std::signal(SIGSEGV, sigsegv_handler);
    std::set_terminate(terminate_handler);

    // don't set default value here
    struct AdbfsmOpt
    {
        const char* m_serial    = nullptr;
        const char* m_log_level = nullptr;
        const char* m_log_file  = nullptr;
        int         m_cachesize = 500;
        int         m_rescan    = false;
        int         m_help      = false;
        int         m_full_help = false;
    };

    fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt adbfsm_opt_spec[] = {
        // clang-format off
        { "--serial=%s",    offsetof(AdbfsmOpt, m_serial),    true },
        { "--log-level=%s", offsetof(AdbfsmOpt, m_log_level), true },
        { "--log-file=%s",  offsetof(AdbfsmOpt, m_log_file),  true },
        { "--cachesize=%d", offsetof(AdbfsmOpt, m_cachesize), true },
        { "--rescan",       offsetof(AdbfsmOpt, m_rescan),    true },
        { "-h",             offsetof(AdbfsmOpt, m_help),      true },
        { "--help",         offsetof(AdbfsmOpt, m_help),      true },
        { "--full-help",    offsetof(AdbfsmOpt, m_full_help), true },
        // clang-format on
        FUSE_OPT_END,
    };

    auto show_help = [&](bool cerr) {
        auto out = cerr ? stderr : stdout;
        fmt::print(out, "usage: {} [options] <mountpoint>\n\n", argv[0]);
        fmt::print(
            out,
            "Options for adbfsm:\n"
            "    --serial=<s>        serial number of the device to mount\n"
            "    --log-level=<l>     log level to use (default: warn)\n"
            "    --log-file=<f>      log file to write to (default: - for stdout)\n"
            "    --cachesize=<n>     maximum size of the cache in MB (default: 500)\n"
            "    --rescan            perform rescan every (idk)\n"
            "    --help              show this help message\n"
            "    --full-help         show full help message (includes libfuse options)\n\n"
        );
    };

    auto adbfsm_opt = AdbfsmOpt{
        .m_log_level = ::strdup("warn"),
        .m_log_file  = ::strdup("-"),
    };

    if (fuse_opt_parse(&args, &adbfsm_opt, adbfsm_opt_spec, NULL) != 0) {
        fmt::println(stderr, "error: failed to parse options\n");
        fmt::println(stderr, "try '{} --help' for more information", argv[0]);
        fmt::println(stderr, "try '{} --fuse-help' for full information", argv[0]);
        return 1;
    }

    if (adbfsm_opt.m_help) {
        show_help(false);
        fuse_opt_free_args(&args);
        return 0;
    } else if (adbfsm_opt.m_full_help) {
        show_help(false);
        fmt::println(stdout, "Options for libfuse:");
        fuse_lib_help(&args);
        fuse_opt_free_args(&args);
        return 0;
    }

    auto log_level = parse_level_str(adbfsm_opt.m_log_level);
    if (not log_level.has_value()) {
        fmt::println(stderr, "error: invalid log level '{}'", adbfsm_opt.m_log_level);
        fmt::println(stderr, "valid log levels: trace, debug, info, warn, error, critical, off");
        fuse_opt_free_args(&args);
        return 1;
    }

    adbfsm::log::init(*log_level, adbfsm_opt.m_log_file);
    adbfsm::log_i({ "starting adb server..." });

    if (auto serv = adbfsm::cmd::exec({ "adb", "start-server" }); serv.returncode != 0) {
        fmt::println(stderr, "error: failed to start adb server, make sure adb is installed and in PATH");
        fmt::println(stderr, "stderr:\n{}", serv.cerr);
        return 1;
    }

    if (adbfsm_opt.m_serial == nullptr) {
        auto serial = get_serial();
        if (serial.empty()) {
            fmt::println(stderr, "error: no device found, make sure a device is connected and authorized");
            fuse_opt_free_args(&args);
            return 1;
        }
        adbfsm_opt.m_serial = ::strdup(serial.c_str());
    } else if (auto status = check_serial(adbfsm_opt.m_serial); status != SerialStatus::Device) {
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

    auto ret = fuse_main(args.argc, args.argv, &adbfsm::operations, (void*)&data);
    fuse_opt_free_args(&args);

    // on invalid argument (1) and no mount point specified (2)
    if (ret == 1 or ret == 2) {
        show_help(true);
    }

    return ret;
}
