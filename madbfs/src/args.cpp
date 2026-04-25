#include "madbfs/args.hpp"

#include "madbfs/adb.hpp"
#include "madbfs/cmd.hpp"

#include <madbfs-common/util/split.hpp>

#include <fmt/base.h>
#include <fmt/ranges.h>
#include <linr/read.hpp>

#include <limits>

namespace
{
    using namespace madbfs;

    /**
     * @brief Check device status specified by its serial is exists.
     *
     * @param serial Serial number of the device.
     *
     * @return Device status.
     */
    inline Await<adb::DeviceStatus> check_serial(Str serial)
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
    inline Await<String> get_serial()
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

        fmt::println("[madbfs] multiple devices detected,");
        for (auto i : madbfs::sv::iota(0u, devices.size())) {
            fmt::println("         - {}: {}", i + 1, devices[i].serial);
        }

        auto choice = 1uz;
        while (true) {
            auto input = linr::read<usize>("[madbfs] please specify which one you would like to use: ");
            if (input and input.value() > 0 and input.value() <= devices.size()) {
                choice = *input;
                break;
            } else if (not input and linr::is_stream_error(input.error())) {
                fmt::println("\n[madbfs] stdin closed, aborting.");
                std::exit(1);    // I don't think there is an easy way out of this but exit
            } else {
                fmt::println("[madbfs] invalid choice, enter a number between 1 - {}: ", devices.size());
                continue;
            }
        }
        fmt::println("[madbfs] using serial '{}'", devices[choice - 1].serial);

        co_return devices[choice - 1].serial;
    }

    /**
     * @brief Search for server binary file.
     *
     * @param exec_path Current executable path.
     * @param server_name Name of the server file.
     *
     * @return Path to server binary if found otherwise `std::nullopt`.
     */
    inline Opt<std::filesystem::path> get_server_path(std::filesystem::path exec_path, Str server_name)
    {
        namespace fs = std::filesystem;

        auto candidates = Vec<fs::path>{};
        candidates.push_back(fs::current_path() / server_name);

        // search in PATH
        if (exec_path.filename() == exec_path and not exec_path.string().starts_with("./")) {
            if (auto path_env = ::getenv("PATH"); path_env != nullptr) {
                auto splitter = util::StringSplitter{ path_env, ':' };
                while (auto path = splitter.next()) {
                    auto file = fs::path{ *path } / exec_path.filename();
                    if (not fs::exists(file)) {
                        continue;
                    }
                    candidates.push_back(file.parent_path() / server_name);
                    while (fs::is_symlink(file)) {
                        auto read = fs::read_symlink(file);
                        if (read.is_relative()) {
                            read = file.parent_path() / read;
                        }
                        file = read;
                        candidates.push_back(file.parent_path() / server_name);
                    }
                }
            }
        } else {
            auto file = fs::absolute(exec_path);
            candidates.push_back(file.parent_path() / server_name);
            while (fs::is_symlink(file)) {
                auto read = fs::read_symlink(file);
                if (read.is_relative()) {
                    read = file.parent_path() / read;
                }
                file = read;
                candidates.push_back(file.parent_path() / server_name);
            }
        }

        auto file = fs::path{};
        for (auto candidate : candidates) {
            if (fs::exists(candidate) and fs::is_regular_file(candidate)) {
                file = candidate;
                return file;
            }
            fmt::println("[madbfs] candidate not exist or not regular file: {}", candidate.c_str());
        }

        return std::nullopt;
    }
}

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
            "    --server=<path>        path to server file\n"
            "                             (if omitted will search the file automatically)\n"
            "                             (must have the same arch as your phone)\n"
            "    --log-level=<enum>     log level to use\n"
            "                             (default: \"warning\")\n"
            "                             (enum: {0:n})\n"
            "    --log-file=<path>      log file to write to\n"
            "                             (default: \"-\" for stdout)\n"
            "    --cache-size=<int>     maximum size of the cache in MiB\n"
            "                             (default: 256)\n"
            "                             (minimum: 128)\n"
            "                             (value will be rounded up to the next power of 2)\n"
            "    --page-size=<int>      page size for cache & transfer in KiB\n"
            "                             (default: 128)\n"
            "                             (minimum: 64)\n"
            "                             (maximum: 4096)\n"
            "                             (value will be rounded up to the next power of 2)\n"
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
            "    --adb-only             don't launch server and don't try to connect\n",
            log::level_names
        );

        fmt::println(stdout, "\nOptions for libfuse:");
        ::fuse_cmdline_help();
        ::fuse_lowlevel_help();
    };

    Await<ParseResult> parse(int argc, char** argv)
    {
        fuse_args args = FUSE_ARGS_INIT(argc, argv);

        auto opts       = fuse_cmdline_opts{};
        auto mountpoint = String{};

        // NOTE: early parse to check whether mount point exists or help/version flag is provided. the
        // resultant opts will not be used and args will be parsed again in fuse_main later.
        {
            if (::fuse_parse_cmdline(&args, &opts) != 0) {
                fmt::println(stderr, "error: failed to parse options\n");
                show_help(argv[0]);
                ::fuse_opt_free_args(&args);
                co_return ParseResult{ 1 };
            }

            if (opts.show_help) {
                show_help(argv[0]);
                ::fuse_opt_free_args(&args);
                ::free(opts.mountpoint);
                co_return ParseResult{ 0 };
            }

            if (opts.show_version) {
                fmt::println("madbfs version {}", MADBFS_VERSION_STRING);
                fmt::println("FUSE library version {}", ::fuse_pkgversion());
                ::fuse_lowlevel_version();
                ::fuse_opt_free_args(&args);
                ::free(opts.mountpoint);
                co_return ParseResult{ 1 };
            }

            if (opts.mountpoint == nullptr) {
                fmt::println(stderr, "error: no mountpoint specified");
                show_help(argv[0]);
                ::fuse_opt_free_args(&args);
                co_return ParseResult{ 2 };
            } else {
                mountpoint = opts.mountpoint;
                ::free(opts.mountpoint);
            }

            // recreate args
            ::fuse_opt_free_args(&args);
            args = FUSE_ARGS_INIT(argc, argv);
        }

        // NOTE: these strings must be malloc-ed since fuse_opt_parse will free them
        auto madbfs_opt = MadbfsOpt{
            .log_level = ::strdup("warning"),
            .log_file  = ::strdup("-"),
        };

        if (fuse_opt_parse(&args, &madbfs_opt, madbfs_opt_spec.data(), NULL) != 0) {
            fmt::println(stderr, "error: failed to parse options\n");
            show_help(argv[0]);
            co_return ParseResult{ 1 };
        }

        if (madbfs_opt.cache_size <= 0) {
            fmt::println(stderr, "error: cache size must be positive");
            co_return ParseResult{ 1 };
        }

        if (madbfs_opt.page_size <= 0) {
            fmt::println(stderr, "error: page size must be positive");
            co_return ParseResult{ 1 };
        }

        if (madbfs_opt.port > std::numeric_limits<u16>::max() or madbfs_opt.port <= 0) {
            fmt::println("[madbfs] invalid port {}", madbfs_opt.port);
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 1 };
        }

        fmt::println("[madbfs] checking adb availability...");
        if (auto status = co_await adb::start_server(); not status) {
            fmt::println(stderr, "\nerror: failed to start adb server [{}].", err_msg(status.error()));
            fmt::println(stderr, "\nnote: make sure adb is installed and in PATH.");
            fmt::println(stderr, "note: make sure phone debugging permission is enabled.");
            fmt::println(stderr, "      phone with its screen locked might denies adb connection.");
            fmt::println(stderr, "      you might need to unlock your device first to be able to use adb.");
            co_return ParseResult{ 1 };
        }

        auto log_level = log::level_from_str(madbfs_opt.log_level);
        if (not log_level.has_value()) {
            fmt::println(stderr, "error: invalid log level '{}'", madbfs_opt.log_level);
            fmt::println(stderr, "       valid log levels: {}", log::level_names);
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 1 };
        }

        if (madbfs_opt.serial == nullptr) {
            if (auto serial = ::getenv("ANDROID_SERIAL"); serial != nullptr) {
                fmt::println("[madbfs] using serial '{}' from env variable 'ANDROID_SERIAL'", serial);
                madbfs_opt.serial = ::strdup(serial);
            } else if (auto serial = co_await get_serial(); not serial.empty()) {
                madbfs_opt.serial = ::strdup(serial.c_str());
            } else {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                ::fuse_opt_free_args(&args);
                co_return ParseResult{ 1 };
            }
        }

        if (auto dev = co_await check_serial(madbfs_opt.serial); dev != adb::DeviceStatus::Device) {
            fmt::println(stderr, "error: serial '{} 'is not valid ({})", madbfs_opt.serial, to_string(dev));
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 1 };
        }

        auto port       = static_cast<u16>(madbfs_opt.port);
        auto connection = Connection{ connection::AdbOnly{} };

        if (madbfs_opt.adb_only) {
            connection = connection::AdbOnly{};
            fmt::println("[madbfs] adb-only flag specified, won't launch server and won't try to connect");
        } else if (madbfs_opt.no_server) {
            connection = connection::NoServer{ .port = port };
            fmt::println("[madbfs] no-server flag specified, won't launch server but will try to connect");
        } else if (madbfs_opt.server == nullptr) {
            auto exe = std::filesystem::path{ argv[0] == nullptr ? "madbfs" : argv[0] };
            auto abi = co_await cmd::exec(
                { "adb", "-s", madbfs_opt.serial, "shell", "getprop", "ro.product.cpu.abi" }
            );

            auto server = Opt<std::filesystem::path>{};

            if (not abi) {
                fmt::println("[madbfs] the device's Android ABI can't be queried");
            } else {
                fmt::println("[madbfs] the device is running with Android ABI '{}'", util::strip(*abi));
                auto server_name = fmt::format("madbfs-server-{}", util::strip(*abi));
                fmt::println("[madbfs] server is not specified, attempting to search '{}'...", server_name);
                server = get_server_path(exe, server_name);
            }

            if (not server) {
                constexpr auto server_name = "madbfs-server";
                fmt::println("[madbfs] trying to find '{}'...", server_name);
                server = get_server_path(exe, server_name);
            }

            if (not server) {
                fmt::println("[madbfs] can't find server, fallback to no-server");
                connection = connection::NoServer{ .port = port };
            } else {
                fmt::println("[madbfs] server is found: {:?}", server->c_str());
                connection = connection::Server{ .path = *server, .port = port };
            }
        } else {
            auto path = std::filesystem::absolute(madbfs_opt.server);
            if (not std::filesystem::exists(path)) {
                fmt::println("[madbfs] {:?} does not exist, fallback to no-server", path.c_str());
                connection = connection::NoServer{ .port = port };
            } else if (not std::filesystem::is_regular_file(path)) {
                fmt::println("[madbfs] {:?} is not a regular file, fallback to no-server", path.c_str());
                connection = connection::NoServer{ .port = port };
            } else {
                fmt::println("[madbfs] server path is set to {:?}", path.c_str());
                connection = connection::Server{ .path = path, .port = port };
            }
        }

        // if logfile is set to stdout but not in foreground mode, ignore it.
        auto log_file = std::strcmp(madbfs_opt.log_file, "-") == 0 and not opts.foreground
                          ? ""
                          : madbfs_opt.log_file;

        auto cache_size = std::max(std::bit_ceil(static_cast<usize>(madbfs_opt.cache_size)), 128uz);
        auto page_size  = std::clamp(std::bit_ceil(static_cast<usize>(madbfs_opt.page_size)), 64uz, 4096uz);

        co_return ParseResult::Opt{
            .opt = {
                .mount      = std::move(mountpoint),
                .serial     = madbfs_opt.serial,
                .connection = connection,
                .log_level  = log_level.value(),
                .log_file   = log_file,
                .cachesize  = cache_size,
                .pagesize   = page_size,
                .ttl        = madbfs_opt.ttl,
                .timeout    = madbfs_opt.timeout,
            },
            .args = args,
        };
    }
}
