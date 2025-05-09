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
        const char* serial    = nullptr;
        const char* log_level = nullptr;
        const char* log_file  = nullptr;
        int         cachesize = 512;    // in MiB
        int         pagesize  = 128;    // in KiB
        int         rescan    = false;
        int         help      = false;
        int         full_help = false;
    };

    struct ParsedOpt
    {
        String                    serial;
        spdlog::level::level_enum log_level;
        String                    log_file;
        usize                     cachesize;
        usize                     pagesize;
        bool                      rescan;
    };

    struct ParseResult
    {
        // clang-format off
        struct Opt  { ParsedOpt opt; fuse_args args; };
        struct Exit { int status; };

        ParseResult(Opt opt)    : result{ std::move(opt) } {}
        ParseResult(int status) : result{ Exit{ status } } {}

        bool is_opt()  const { return std::holds_alternative<Opt>(result);  }
        bool is_exit() const { return std::holds_alternative<Exit>(result); }

        Opt&&  opt()  && { return std::move(std::get<Opt>(result));  }
        Exit&& exit() && { return std::move(std::get<Exit>(result)); }

        Var<Opt, Exit> result;
        // clang-format on
    };

    static constexpr auto adbfsm_opt_spec = Array<fuse_opt, 9>{ {
        // clang-format off
        { "--serial=%s",    offsetof(AdbfsmOpt, serial),    true },
        { "--loglevel=%s",  offsetof(AdbfsmOpt, log_level), true },
        { "--logfile=%s",   offsetof(AdbfsmOpt, log_file),  true },
        { "--cachesize=%d", offsetof(AdbfsmOpt, cachesize), true },
        { "--pagesize=%d",  offsetof(AdbfsmOpt, pagesize),  true },
        { "-h",             offsetof(AdbfsmOpt, help),      true },
        { "--help",         offsetof(AdbfsmOpt, help),      true },
        { "--full-help",    offsetof(AdbfsmOpt, full_help), true },
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
            .serial    = get_serial_env(),
            .log_level = ::strdup("warn"),
            .log_file  = ::strdup("-"),
        };

        if (fuse_opt_parse(&args, &adbfsm_opt, adbfsm_opt_spec.data(), NULL) != 0) {
            fmt::println(stderr, "error: failed to parse options\n");
            fmt::println(stderr, "try '{} --help' for more information", argv[0]);
            fmt::println(stderr, "try '{} --full-help' for full information", argv[0]);
            return 1;
        }

        if (adbfsm_opt.help) {
            show_help(argv[0], false);
            ::fuse_opt_free_args(&args);
            return 0;
        } else if (adbfsm_opt.full_help) {
            show_help(argv[0], false);
            fmt::println(stdout, "\nOptions for libfuse:");
            ::fuse_cmdline_help();
            ::fuse_lowlevel_help();
            ::fuse_opt_free_args(&args);
            return 0;
        }

        auto log_level = parse_level_str(adbfsm_opt.log_level);
        if (not log_level.has_value()) {
            fmt::println(stderr, "error: invalid log level '{}'", adbfsm_opt.log_level);
            fmt::println(stderr, "valid log levels: trace, debug, info, warn, error, critical, off");
            ::fuse_opt_free_args(&args);
            return 1;
        }

        if (adbfsm_opt.serial == nullptr) {
            auto serial = get_serial();
            if (serial.empty()) {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                ::fuse_opt_free_args(&args);
                return 1;
            }
            adbfsm_opt.serial = ::strdup(serial.c_str());
        } else if (auto status = check_serial(adbfsm_opt.serial); status != data::DeviceStatus::Device) {
            fmt::println(
                stderr, "error: serial '{} 'is not valid ({})", adbfsm_opt.serial, to_string(status)
            );
            ::fuse_opt_free_args(&args);
            return 1;
        }

        return ParseResult::Opt{
            .opt = {
                .serial    = adbfsm_opt.serial,
                .log_level = log_level.value(),
                .log_file  = adbfsm_opt.log_file,
                .cachesize = std::bit_ceil(std::max(static_cast<usize>(adbfsm_opt.cachesize), 128uz)),
                .pagesize  = std::bit_ceil(std::max(static_cast<usize>(adbfsm_opt.pagesize), 64uz)),
                .rescan    = static_cast<bool>(adbfsm_opt.rescan),
            },
            .args = args,
        };
    }
}
