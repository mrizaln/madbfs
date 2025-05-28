#include "madbfs-server/server.hpp"

#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <thread>

std::atomic<bool> g_interrupt = false;

void sig_handler(int sig)
{
    fmt::println("signal {} raised!", sig);

    g_interrupt = true;
    g_interrupt.notify_all();

    // std::signal(sig, SIG_DFL);
    // std::raise(sig);
}

int main(int argc, char** argv)
try {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    auto debug = false;
    auto port  = madbfs::u16{ 12345 };

    for (auto i = 0; i < argc; ++i) {
        auto arg = madbfs::Str{ argv[i] };
        if (arg == "--help" or arg == "-h") {
            fmt::println("{} [--port PORT] [--debug]\n", argv[0]);
            fmt::println("  --port PORT       Port number the server listen on (default: 12345");
            fmt::println("  --debug           Enable debug logging.");
            return 0;
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                spdlog::error("expecting port number after '--port' argument");
                return 1;
            }

            arg = madbfs::Str{ argv[++i] };

            auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), port);
            if (ec != std::errc{}) {
                auto msg = std::make_error_code(ec).message();
                spdlog::error("failed to parse port number '{}': {}", arg, msg);
                return 1;
            } else if (ptr != arg.data() + arg.size()) {
                spdlog::error("failed to parse port number '{}': invalid trailing characters", arg);
                return 1;
            }
        }
    }

    if (debug) {
        spdlog::set_level(spdlog::level::debug);
    }

    auto context = madbfs::async::Context{};
    auto server  = madbfs::server::Server{ context, port };    // may throw, just terminate

    madbfs::async::spawn(context, server.run(), madbfs::async::detached);
    auto thread = std::thread{ [&] { context.run(); } };

    g_interrupt.wait(false);

    server.stop();
    thread.join();

    spdlog::info("server exited normally");

    return 0;
} catch (std::exception& e) {
    spdlog::critical("exception: {}", e.what());
    return 1;
}
