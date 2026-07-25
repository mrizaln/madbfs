#include "madbfs/args.hpp"

#include "madbfs/adb.hpp"
#include "madbfs/cmd.hpp"

#include <madbfs-gen/version.hpp>

#include <madbfs-common/util/defer.hpp>
#include <madbfs-common/util/split.hpp>

#include <fmt/base.h>
#include <fmt/ranges.h>
#include <linr/read.hpp>

#include <limits>

using namespace madbfs;
using namespace madbfs::args;

// helper functions/classes
namespace
{
    /**
     * @class MadbfsOpt
     *
     * @brief Madbfs options.
     *
     * Don't set default value here for string, set them in the `parse()` function. This structure is used
     * with `fuse_opt`, parsed opt with correct values is stored in `ParsedOpt` instead.
     */
    struct MadbfsOpt
    {
        const char* serial     = nullptr;
        const char* root       = nullptr;
        const char* log_level  = nullptr;
        const char* log_file   = nullptr;
        int         cache_size = 256;    // in MiB
        int         page_size  = 128;    // in KiB
        int         ttl        = 60;     // in seconds
        int         timeout    = 2;      // in seconds
        int         port       = 23237;
        int         no_server  = false;
        int         adb_only   = false;
        int         no_cache   = false;
        int         push       = false;

        ~MadbfsOpt()
        {
            ::free((void*)serial);
            ::free((void*)root);
            ::free((void*)log_level);
            ::free((void*)log_file);
        }
    };

    static constexpr auto madbfs_opt_spec = std::to_array<fuse_opt>({
        // clang-format off
        { "--serial=%s",     offsetof(MadbfsOpt, serial),     true },
        { "--root=%s",       offsetof(MadbfsOpt, root),       true },
        { "--log-level=%s",  offsetof(MadbfsOpt, log_level),  true },
        { "--log-file=%s",   offsetof(MadbfsOpt, log_file),   true },
        { "--cache-size=%d", offsetof(MadbfsOpt, cache_size), true },
        { "--page-size=%d",  offsetof(MadbfsOpt, page_size),  true },
        { "--ttl=%d",        offsetof(MadbfsOpt, ttl),        true },
        { "--timeout=%d",    offsetof(MadbfsOpt, timeout),    true },
        { "--port=%d",       offsetof(MadbfsOpt, port),       true },
        { "--no-server",     offsetof(MadbfsOpt, no_server),  true },
        { "--adb-only",      offsetof(MadbfsOpt, adb_only),   true },
        { "--no-cache",      offsetof(MadbfsOpt, no_cache),   true },
        { "--push-server",   offsetof(MadbfsOpt, push),       true },
        // clang-format on
        FUSE_OPT_END,
    });

    /**
     * @brief Check device status specified by its serial is exists.
     *
     * @param serial Serial number of the device.
     *
     * @return Device status.
     */
    Await<adb::DeviceStatus> check_serial(Str serial)
    {
        if (auto maybe_devices = co_await adb::list_devices(); maybe_devices.has_value()) {
            auto found = sr::find(*maybe_devices, serial, &adb::Device::serial);
            if (found != maybe_devices->end()) {
                co_return found->status;
            }
        }
        co_return adb::DeviceStatus::Unknown;
    }

    /**
     * @brief Query for serial number of adb device.
     *
     * @return Serial number.
     *
     * This function may prompt the user (stdin) if there are multiple device connected to the computer.
     */
    Await<String> get_serial()
    {
        auto maybe_devices = co_await adb::list_devices();
        if (not maybe_devices.has_value()) {
            co_return "";
        }
        auto devices = *maybe_devices    //
                     | sv::filter([](auto&& d) { return d.status == adb::DeviceStatus::Device; })
                     | sr::to<Vec<adb::Device>>();

        if (devices.empty()) {
            co_return "";
        } else if (devices.size() == 1) {
            fmt::println("[madbfs] only one device found, using serial '{}'", devices[0].serial);
            co_return devices[0].serial;
        }

        fmt::println("[madbfs] multiple devices detected:");
        for (auto i : sv::iota(0u, devices.size())) {
            fmt::println("\t- {}: {}", i + 1, devices[i].serial);
        }

        auto choice = 1uz;
        fmt::print("[madbfs] please specify which one you would like to use: ");

        while (true) {
            auto input = linr::read<usize>();
            if (input and input.value() > 0 and input.value() <= devices.size()) {
                choice = *input;
                break;
            } else if (not input and linr::is_stream_error(input.error())) {
                fmt::println(stderr, "\nerror: stdin closed, aborting.");
                std::exit(1);    // I don't think there is an easy way out of this but exit
            } else {
                fmt::print(stderr, "error: invalid choice, enter number between 1 - {}: ", devices.size());
                continue;
            }
        }
        fmt::println("[madbfs] using serial '{}'", devices[choice - 1].serial);

        co_return devices[choice - 1].serial;
    }

    /**
     * @brief Create RAII string from C-style string (may be null).
     *
     * @param string Char array
     *
     * This function is necessary because apparently, it is illegal to pass nullptr to std::string's
     * constructor. I am baffled as well.
     */
    String to_string(const char* string)
    {
        return string ? string : "";
    }

    /**
     * @brief Parse and validate arguments into madbfs mount options.
     *
     * @param madbfs_opt Parsed madbfs options (not validated yet).
     * @param args Original fuse arguments.
     * @param mountpoint Mountpoint of madbfs.
     * @param foreground Whether madbfs will be launched is in foreground mode.
     */
    Await<ParseResult> into_mount_opt(
        const MadbfsOpt& madbfs_opt,
        FuseArgs         args,
        String           mountpoint,
        bool             foreground
    )
    {
        if (madbfs_opt.cache_size <= 0) {
            fmt::println(stderr, "error: cache size must be positive");
            co_return 1;
        }

        if (madbfs_opt.page_size <= 0) {
            fmt::println(stderr, "error: page size must be positive");
            co_return 1;
        }

        if (madbfs_opt.port > std::numeric_limits<u16>::max() or madbfs_opt.port <= 0) {
            fmt::println("[madbfs] invalid port {}", madbfs_opt.port);
            co_return 1;
        }

        fmt::println("[madbfs] checking adb availability...");
        if (auto status = co_await adb::start_server(); not status) {
            fmt::println(stderr, "\nerror: failed to start adb server: {}", err_msg(status.error()));
            fmt::println(stderr, "\nnote: make sure adb is installed and in PATH.");
            fmt::println(stderr, "note: make sure phone debugging permission is enabled.");
            fmt::println(stderr, "      phone with its screen locked might denies adb connection.");
            fmt::println(stderr, "      you might need to unlock your device first to be able to use adb.");
            co_return 1;
        }

        auto log_level = log::level_from_str(madbfs_opt.log_level);
        if (not log_level.has_value()) {
            fmt::println(stderr, "error: invalid log level '{}'", madbfs_opt.log_level);
            fmt::println(stderr, "       valid log levels: {}", log::level_names);
            co_return 1;
        }

        auto serial = to_string(madbfs_opt.serial);
        if (serial.empty()) {
            if (auto env = ::getenv("ANDROID_SERIAL"); env != nullptr) {
                fmt::println("[madbfs] using serial '{}' from env variable 'ANDROID_SERIAL'", env);
                serial = env;
            } else if (auto res = co_await get_serial(); not res.empty()) {
                serial = std::move(res);
            } else {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                co_return 1;
            }
        }

        if (auto dev = co_await check_serial(serial); dev != adb::DeviceStatus::Device) {
            fmt::println(stderr, "error: serial '{}' is not valid ({})", serial, to_string(dev));
            co_return 1;
        }

        auto root = path::PathBuf{};
        if (madbfs_opt.root) {
            auto path = String{ madbfs_opt.root };
            if (path.empty() or path.front() != '/') {
                fmt::println(stderr, "error: root path '{}' is not valid", path);
                co_return 1;
            }

            auto quoted = fmt::format("\"{}\"", path);

            auto dir = co_await cmd::exec({ "adb", "-s", serial, "shell", "test", "-d", quoted });
            if (not dir) {
                fmt::println(stderr, "error: root path '{}' is not a directory or not exists", path);
                co_return 1;
            }

            auto real = co_await cmd::exec({ "adb", "-s", serial, "shell", "realpath", quoted });
            if (not real) {
                fmt::println(stderr, "error: failed to resolve '{}' path: {}", path, err_msg(real.error()));
                co_return 1;
            }

            root = path::create_buf(String{ util::strip(*real) }).value();
            fmt::println("[madbfs] root resolved: '{}' -> '{}'", path, root);
        }

        auto port       = static_cast<u16>(madbfs_opt.port);
        auto connection = Connection{ AdbOnly{} };

        if (madbfs_opt.adb_only) {
            fmt::println("[madbfs] adb-only flag specified, won't launch server and won't try to connect");
        } else if (madbfs_opt.no_server) {
            connection = NoServer{ .port = port };
            fmt::println("[madbfs] no-server flag specified, won't launch server but will try to connect");
        } else if (auto abi = co_await adb::get_abi(serial); not abi) {
            fmt::println("[madbfs] device ABI query failed: {} (fallback to adb)", err_msg(abi.error()));
        } else {
            connection = Server{ .abi = *abi, .port = port };
        }

        // if logfile is set to stdout but not in foreground mode, ignore it.
        auto log_file = to_string(madbfs_opt.log_file);
        if (log_file == "-" and not foreground) {
            log_file = "";
        }

        auto caching = Opt<Caching>{};
        if (not madbfs_opt.no_cache) {
            caching = Caching{
                .cachesize = std::max(std::bit_ceil(static_cast<usize>(madbfs_opt.cache_size)), 32uz),
                .pagesize = std::clamp(std::bit_ceil(static_cast<usize>(madbfs_opt.page_size)), 64uz, 4096uz),
            };
        }

        co_return MountOpt{
            .args       = std::move(args),
            .mount      = std::move(mountpoint),
            .serial     = std::move(serial),
            .root       = std::move(root),
            .connection = connection,
            .caching    = caching,
            .log_level  = log_level.value(),
            .log_file   = std::move(log_file),
            .ttl        = madbfs_opt.ttl,
            .timeout    = madbfs_opt.timeout,
        };
    }

    /**
     * @brief Parse into madbfs push options.
     *
     * @param serial Device serial.
     */
    Await<ParseResult> into_push_opt(String serial)
    {
        if (serial.empty()) {
            if (auto env = ::getenv("ANDROID_SERIAL"); env != nullptr) {
                fmt::println("[madbfs] using serial '{}' from env variable 'ANDROID_SERIAL'", env);
                serial = env;
            } else if (auto res = co_await get_serial(); not res.empty()) {
                serial = std::move(res);
            } else {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                co_return 1;
            }
        }

        if (auto dev = co_await check_serial(serial); dev != adb::DeviceStatus::Device) {
            fmt::println(stderr, "error: serial '{}' is not valid ({})", serial, to_string(dev));
            co_return 1;
        }

        co_return args::PushOpt{ std::move(serial) };
    }
}

// args.hpp impl
namespace madbfs::args
{
    void show_help(const char* prog)
    {
        fmt::print(stdout, "usage: {} [options] <mountpoint>\n\n", prog);
        fmt::print(
            stdout,
            "Options for madbfs:\n"
            "    --serial=<str>         serial number of the device to mount\n"
            "                             (you can omit this [detection is similar to adb])\n"
            "                             (will prompt if more than one device exists)\n"
            "    --root=<path>          directory to mount on device as root of the filesystem\n"
            "                             (by default madbfs mounts the root path of the device)\n"
            "                             (the path must be absolute and points to an existing path)\n"
            "                             (if the path is a symlink, it will be resolved first)\n"
            "    --log-level=<enum>     log level to use\n"
            "                             (default: \"warning\")\n"
            "                             (enum: {0:n})\n"
            "    --log-file=<path>      log file to write to\n"
            "                             (default: \"-\" for stdout)\n"
            "    --cache-size=<int>     maximum size of the cache in MiB\n"
            "                             (default: 256)\n"
            "                             (minimum: 32)\n"
            "                             (value will be rounded up to the next power of 2)\n"
            "                             (ignored if 'no-cache' is provided)\n"
            "    --page-size=<int>      page size for cache & transfer in KiB\n"
            "                             (default: 128)\n"
            "                             (minimum: 64)\n"
            "                             (maximum: 4096)\n"
            "                             (value will be rounded up to the next power of 2)\n"
            "                             (ignored if 'no-cache' is provided)\n"
            "    --ttl=<int>            set the TTL of the stat cache of the filesystem in seconds\n"
            "                             (default: 60)\n"
            "                             (set to 0 to disable it)\n"
            "    --timeout=<int>        set the timeout of every remote operation\n"
            "                             (default: 2)\n"
            "                             (set to 0 to disable it)\n"
            "    --port=<int>           set the port number the server will listen on\n"
            "                             (default: 23237)\n"
            "    --no-server            don't launch server\n"
            "                             (will still attempt to connect to specified port)\n"
            "                             (fall back to adb shell calls if connection failed)\n"
            "                             (useful for debugging the server)\n"
            "    --adb-only             don't launch server and don't try to connect\n"
            "    --no-cache             don't use data caching\n"
            "    --push-server          push the server binary to your device\n",
            log::level_names
        );

        fmt::println(stdout, "\nOptions for libfuse:");
        ::fuse_cmdline_help();
        ::fuse_lowlevel_help();
    };

    Await<ParseResult> parse(int argc, char** argv)
    {
        auto args       = FuseArgs{ argc, argv };
        auto opts       = fuse_cmdline_opts{};
        auto mountpoint = Opt<String>{};

        // NOTE: these strings must be malloc-ed since fuse_opt_parse might free them
        auto madbfs_opt = MadbfsOpt{
            .log_level = ::strdup("warning"),
            .log_file  = ::strdup("-"),
        };

        {
            // early parse
            if (::fuse_parse_cmdline(&args.inner(), &opts) != 0) {
                fmt::println(stderr, "error: failed to parse options");
                co_return ParseResult{ 1 };
            }
            auto free = util::defer([&] { ::free(opts.mountpoint); });

            if (opts.show_help) {
                show_help(argv[0]);
                co_return 0;
            }

            if (opts.show_version) {
                fmt::println("madbfs version {}", version::get_version_full());
                fmt::println("FUSE library version {}", ::fuse_pkgversion());
                co_return 0;
            }

            if (opts.mountpoint != nullptr) {
                mountpoint = opts.mountpoint;
            }

            // recreate args
            args = FuseArgs(argc, argv);
        }

        if (::fuse_opt_parse(&args.inner(), &madbfs_opt, madbfs_opt_spec.data(), NULL) != 0) {
            fmt::println(stderr, "error: failed to parse options");
            co_return 1;
        }

        if (madbfs_opt.push) {
            co_return co_await into_push_opt(to_string(madbfs_opt.serial));
        }

        if (not mountpoint) {
            fmt::println(stderr, "error: no mountpoint specified");
            fmt::println(stderr, "try --help");
            co_return 2;
        }

        co_return co_await into_mount_opt(madbfs_opt, std::move(args), *mountpoint, opts.foreground);
    }
}
