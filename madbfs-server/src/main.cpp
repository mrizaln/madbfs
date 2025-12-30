#include <madbfs-common/log.hpp>
#include <madbfs-common/rpc.hpp>
#include <madbfs-server/server.hpp>

#include <charconv>
#include <csignal>
#include <print>

struct Exit
{
    int ret;
};

struct Args
{
    madbfs::log::Level log_level = madbfs::log::Level::warn;
    madbfs::u16        port      = 12345;
};

std::variant<Exit, Args> parse_args(int argc, char** argv)
{
    using madbfs::log::Level;
    auto args = Args{};

    for (auto i = 1; i < argc; ++i) {
        auto arg = madbfs::Str{ argv[i] };
        if (arg == "--help" or arg == "-h") {
            std::println("{} [--port PORT] [--debug] [--verbose]\n", argv[0]);
            std::println("  --port PORT       Port number the server listen on (default: 12345");
            std::println("  --debug           Enable debug logging.");
            std::println("  --verbose         Enable verbose logging.");
            return Exit{ 0 };
        } else if (arg == "--debug") {
            args.log_level = Level::debug;
        } else if (arg == "--verbose") {
            args.log_level = Level::info;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::println(stderr, "expecting port number after '--port' argument");
                return Exit{ 1 };
            }

            arg = madbfs::Str{ argv[++i] };

            auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), args.port);
            if (ec != std::errc{}) {
                std::println(stderr, "failed to parse port number '{}': {}", arg, madbfs::err_msg(ec));
                return Exit{ 1 };
            } else if (ptr != arg.data() + arg.size()) {
                std::println(stderr, "failed to parse port number '{}': invalid trailing characters", arg);
                return Exit{ 1 };
            }
        } else {
            std::println(stderr, "unknown argument: {}", arg);
            return Exit{ 1 };
        }
    }

    return args;
}

int main(int argc, char** argv)
try {
    auto parsed = parse_args(argc, argv);
    if (parsed.index() == 0) {
        return std::get<0>(parsed).ret;
    }

    auto args = std::get<1>(parsed);
    madbfs::log::init(args.log_level, "-");

    auto context = madbfs::async::Context{};
    auto server  = madbfs::server::Server{ context, args.port };    // may throw

    auto sig_set = madbfs::net::signal_set{ context, SIGINT, SIGTERM };
    sig_set.async_wait([&](auto, auto) { server.stop(); });

    auto task = [&](this auto) -> madbfs::AExpect<void> {
        auto res = co_await server.run();
        sig_set.cancel();
        co_return res;
    };

    std::println(madbfs::rpc::server_ready_string);
    std::fflush(stdout);    // ensure the message is sent

    auto res = madbfs::async::once(context, task());

    madbfs::log_i("server exited normally: {}", madbfs::err_msg(res.error_or({})));
    return 0;
} catch (const std::exception& e) {
    madbfs::log_c("exception: {}", e.what());
    return 1;
} catch (...) {
    madbfs::log_c("exception (unknown type)");
    return 1;
}
