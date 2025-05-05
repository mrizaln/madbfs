#pragma once

#include "adbfsm/data/connection.hpp"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <spdlog/spdlog.h>

#include <iostream>

namespace adbfsm::args
{
    // NOTE: don't set default value here for string, set them in the parse() function
    struct AdbfsmOpt
    {
        const char* m_serial    = nullptr;
        const char* m_log_level = nullptr;
        const char* m_log_file  = nullptr;
        int         m_cachesize = 512;    // in MiB
        int         m_pagesize  = 128;    // in KiB
        int         m_rescan    = false;
        int         m_help      = false;
        int         m_full_help = false;
    };

    struct ParsedOpt
    {
        String                    m_serial;
        spdlog::level::level_enum m_log_level;
        String                    m_log_file;
        usize                     m_cachesize;
        usize                     m_pagesize;
        bool                      m_rescan;
    };

    struct ParseResult
    {
        // clang-format off
        struct Opt  { ParsedOpt m_opt; fuse_args m_fuse_args; };
        struct Exit { int m_status; };

        ParseResult(Opt opt)    : m_result{ std::move(opt) } {}
        ParseResult(int status) : m_result{ Exit{ status } } {}

        bool is_opt()  const { return std::holds_alternative<Opt>(m_result);  }
        bool is_exit() const { return std::holds_alternative<Exit>(m_result); }

        Opt&&  opt()  && { return std::move(std::get<Opt>(m_result));  }
        Exit&& exit() && { return std::move(std::get<Exit>(m_result)); }

        Var<Opt, Exit> m_result;
        // clang-format on
    };

    static constexpr auto adbfsm_opt_spec = Array<fuse_opt, 9>{ {
        // clang-format off
        { "--serial=%s",    offsetof(AdbfsmOpt, m_serial),    true },
        { "--loglevel=%s",  offsetof(AdbfsmOpt, m_log_level), true },
        { "--logfile=%s",   offsetof(AdbfsmOpt, m_log_file),  true },
        { "--cachesize=%d", offsetof(AdbfsmOpt, m_cachesize), true },
        { "--pagesize=%d",  offsetof(AdbfsmOpt, m_pagesize),  true },
        { "-h",             offsetof(AdbfsmOpt, m_help),      true },
        { "--help",         offsetof(AdbfsmOpt, m_help),      true },
        { "--full-help",    offsetof(AdbfsmOpt, m_full_help), true },
        // clang-format on
        FUSE_OPT_END,
    } };

    inline void show_help(const char* prog, bool cerr)
    {
        auto out = cerr ? stderr : stdout;
        fmt::print(out, "usage: {} [options] <mountpoint>\n\n", prog);
        fmt::print(
            out,
            "Options for adbfsm:\n"
            "    --serial=<s>        serial number of the device to mount\n"
            "                          (default: <auto> [detection is similar to adb])\n"
            "    --loglevel=<l>      log level to use (default: warn)\n"
            "    --logfile=<f>       log file to write to (default: - for stdout)\n"
            "    --cachesize=<n>     maximum size of the cache in MiB\n"
            "                          (default: 512)\n"
            "                          (minimum: 128)\n"
            "                          (value will be rounded to the next power of 2)\n"
            "    --pagesize=<n>      page size for cache & transfer in KiB\n"
            "                          (default: 128)\n"
            "                          (minimum: 64)\n"
            "                          (value will be rounded to the next power of 2)\n"
            "    -h   --help         show this help message\n"
            "    --full-help         show full help message (includes libfuse options)\n"
        );
    };

    inline Opt<spdlog::level::level_enum> parse_level_str(Str level)
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

    inline data::DeviceStatus check_serial(Str serial)
    {
        if (auto maybe_devices = data::list_devices(); maybe_devices.has_value()) {
            auto found = sr::find(*maybe_devices, serial, &data::Device::serial);
            if (found != maybe_devices->end()) {
                return found->status;
            }
        }
        return data::DeviceStatus::Unknown;
    }

    inline String get_serial()
    {
        auto maybe_devices = data::list_devices();
        if (not maybe_devices.has_value()) {
            return "";
        }
        auto devices = *maybe_devices                                                                 //
                     | sv::filter([](auto&& d) { return d.status == data::DeviceStatus::Device; })    //
                     | sr::to<Vec<data::Device>>();

        if (devices.empty()) {
            return {};
        } else if (devices.size() == 1) {
            fmt::println("[adbfsm] only one device found, using serial '{}'", devices[0].serial);
            return devices[0].serial;
        }

        fmt::println("[adbfsm] multiple devices detected,");
        for (auto i : adbfsm::sv::iota(0u, devices.size())) {
            fmt::println("         - {}: {}", i + 1, devices[i].serial);
        }
        fmt::print("[adbfsm] please specify which one you would like to use: ");

        auto choice = 1u;
        while (true) {
            std::cin >> choice;
            if (choice > 0 and choice <= devices.size()) {
                break;
            }
            fmt::print("[adbfsm] invalid choice, please enter a number between 1 and {}: ", devices.size());
        }
        fmt::println("[adbfsm] using serial '{}'", devices[choice - 1].serial);

        return devices[choice - 1].serial;
    }

    /**
     * @brief Parse the command line arguments; show help message if needed.
     *
     * @param argc Number of arguments.
     * @param argv Array of arguments.
     *
     * If the return value is `ParseResult::Opt`, the `fuse_args` member must be freed using
     * `fuse_opt_free_args` after use.
     */
    inline ParseResult parse(int argc, char** argv)
    {
        fuse_args args = FUSE_ARGS_INIT(argc, argv);

        auto get_serial_env = []() -> const char* {
            if (auto serial = ::getenv("ANDROID_SERIAL"); serial != nullptr) {
                fmt::println("[adbfsm] using serial '{}' from env variable 'ANDROID_SERIAL'", serial);
                return ::strdup(serial);
            }
            return nullptr;
        };

        // NOTE: these strings must be malloc-ed since fuse_opt_parse will free them
        auto adbfsm_opt = AdbfsmOpt{
            .m_serial    = get_serial_env(),
            .m_log_level = ::strdup("warn"),
            .m_log_file  = ::strdup("-"),
        };

        if (fuse_opt_parse(&args, &adbfsm_opt, adbfsm_opt_spec.data(), NULL) != 0) {
            fmt::println(stderr, "error: failed to parse options\n");
            fmt::println(stderr, "try '{} --help' for more information", argv[0]);
            fmt::println(stderr, "try '{} --full-help' for full information", argv[0]);
            return 1;
        }

        if (adbfsm_opt.m_help) {
            show_help(argv[0], false);
            ::fuse_opt_free_args(&args);
            return 0;
        } else if (adbfsm_opt.m_full_help) {
            show_help(argv[0], false);
            fmt::println(stdout, "\nOptions for libfuse:");
            ::fuse_cmdline_help();
            ::fuse_lowlevel_help();
            ::fuse_opt_free_args(&args);
            return 0;
        }

        auto log_level = parse_level_str(adbfsm_opt.m_log_level);
        if (not log_level.has_value()) {
            fmt::println(stderr, "error: invalid log level '{}'", adbfsm_opt.m_log_level);
            fmt::println(stderr, "valid log levels: trace, debug, info, warn, error, critical, off");
            ::fuse_opt_free_args(&args);
            return 1;
        }

        if (adbfsm_opt.m_serial == nullptr) {
            auto serial = get_serial();
            if (serial.empty()) {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                ::fuse_opt_free_args(&args);
                return 1;
            }
            adbfsm_opt.m_serial = ::strdup(serial.c_str());
        } else if (auto status = check_serial(adbfsm_opt.m_serial); status != data::DeviceStatus::Device) {
            fmt::println(
                stderr, "error: serial '{} 'is not valid ({})", adbfsm_opt.m_serial, to_string(status)
            );
            ::fuse_opt_free_args(&args);
            return 1;
        }

        return ParseResult::Opt{
            .m_opt = {
                .m_serial    = adbfsm_opt.m_serial,
                .m_log_level = log_level.value(),
                .m_log_file  = adbfsm_opt.m_log_file,
                .m_cachesize = std::bit_ceil(std::max(static_cast<usize>(adbfsm_opt.m_cachesize), 128uz)),
                .m_pagesize  = std::bit_ceil(std::max(static_cast<usize>(adbfsm_opt.m_pagesize), 64uz)),
                .m_rescan    = static_cast<bool>(adbfsm_opt.m_rescan),
            },
            .m_fuse_args = args,
        };
    }
}
