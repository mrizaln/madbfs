#include "madbfs-common/log.hpp"
#include "madbfs-common/rpc.hpp"
#include "madbfs-server/server.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <thread>

std::atomic<bool> g_interrupt = false;

void sig_handler(int sig)
{
    madbfs::log_i({ "signal {} raised!" }, sig);

    g_interrupt = true;
    g_interrupt.notify_all();

    // std::signal(sig, SIG_DFL);
    // std::raise(sig);
}

int main(int argc, char** argv)
try {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    auto log_level = spdlog::level::warn;
    auto port      = madbfs::u16{ 12345 };

    for (auto i = 1; i < argc; ++i) {
        auto arg = madbfs::Str{ argv[i] };
        if (arg == "--help" or arg == "-h") {
            fmt::println("{} [--port PORT] [--debug]\n", argv[0]);
            fmt::println("  --port PORT       Port number the server listen on (default: 12345");
            fmt::println("  --debug           Enable debug logging.");
            return 0;
        } else if (arg == "--debug") {
            log_level = spdlog::level::debug;
        } else if (arg == "--verbose") {
            log_level = spdlog::level::info;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                fmt::println(stderr, "expecting port number after '--port' argument");
                return 1;
            }

            arg = madbfs::Str{ argv[++i] };

            auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), port);
            if (ec != std::errc{}) {
                auto msg = std::make_error_code(ec).message();
                fmt::println(stderr, "failed to parse port number '{}': {}", arg, msg);
                return 1;
            } else if (ptr != arg.data() + arg.size()) {
                fmt::println(stderr, "failed to parse port number '{}': invalid trailing characters", arg);
                return 1;
            }
        } else {
            fmt::println(stderr, "unknown argument: {}", arg);
            return 1;
        }
    }

    madbfs::log::init(log_level, "-");

    auto context = madbfs::async::Context{};
    auto server  = madbfs::server::Server{ context, port };    // may throw

    madbfs::async::spawn(context, server.run(), madbfs::async::detached);
    auto thread = std::thread{ [&] { context.run(); } };

    fmt::println(madbfs::rpc::server_ready_string);
    std::fflush(stdout);    // ensure the message is sent

    g_interrupt.wait(false);

    server.stop();
    thread.join();

    spdlog::info("server exited normally");

    return 0;
} catch (std::exception& e) {
    spdlog::critical("exception: {}", e.what());
    return 1;
}
