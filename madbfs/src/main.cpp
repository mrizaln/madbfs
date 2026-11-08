#include "madbfs/args.hpp"
#include "madbfs/cmd.hpp"
#include "madbfs/madbfs.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-gen/version.hpp>

#include <exception>

using namespace madbfs;

void termination()
{
    if (auto e = std::current_exception(); e != nullptr) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& e) {
            log_c(__func__, "> Uncaught exception:\n{}", e.what());
        } catch (...) {
            log_c(__func__, "> Uncaught exception (unknown type)");
        }
    }
    log_c(__func__, "> Terminating");
    log::shutdown();
    std::abort();
}

Await<int> push(const args::PushOpt& opt)
{
    constexpr auto destination = "/data/local/tmp/madbfs-server";

    if (::setenv("ANDROID_SERIAL", opt.serial.c_str(), 1) < 0) {
        fmt::println(stderr, "error: failed to set env variable 'ANDROID_SERIAL' ({})", strerror(errno));
        co_return 1;
    }

    auto abi = co_await adb::get_abi(opt.serial);
    if (not abi) {
        fmt::println(stderr, "error: failed to get abi: {}", err_msg(abi.error()));
        co_return 1;
    }

    auto server_bytes = adb::get_server(*abi);
    auto server_str   = Str{ reinterpret_cast<const char*>(server_bytes.data()), server_bytes.size() };
    auto ofile        = fmt::format("of={}", destination);

    if (auto res = co_await cmd::exec({ "adb", "shell", "dd", ofile }, server_str); not res) {
        fmt::println(stderr, "error: failed to push server: {}", err_msg(res.error()));
        co_return 1;
    }

    if (auto res = co_await cmd::exec({ "adb", "shell", "chmod", "+x", destination }); not res) {
        fmt::println(stderr, "error: failed to update server permission: {}", err_msg(res.error()));
        co_return 1;
    }

    fmt::println("server is pushed to '{}'", destination);
    co_return 0;
}

int mount(args::MountOpt& opt)
{
    if (not log::init(opt.app.log_level, opt.app.log_file)) {
        return 1;
    }

    fmt::println("[madbfs] version is {}", version::get_version_full());
    if (opt.app.caching) {
        fmt::println(
            "[madbfs] mount '{}@{}' [cache={} MiB, page={} KiB]",
            opt.app.serial,
            opt.app.root,
            opt.app.caching->cachesize,
            opt.app.caching->pagesize
        );
    } else {
        fmt::println("[madbfs] mount '{}@{}' [no cache]", opt.app.serial, opt.app.root);
    }

    if (opt.app.log_file != "-") {
        log_i(__func__, "[madbfs] version is {}", version::get_version_full());
        if (opt.app.caching) {
            log_i(
                __func__,
                "[madbfs] mount '{}@{}' at '{}' with cache size {} MiB and page size {} KiB",
                opt.app.serial,
                opt.app.root,
                opt.fuse.mount,
                opt.app.caching->cachesize,
                opt.app.caching->pagesize
            );
        } else {
            log_i(
                __func__,
                "[madbfs] mount '{}@{}' at '{}' with cache disabled",
                opt.app.serial,
                opt.app.root,
                opt.fuse.mount
            );
        }
    }

    fmt::println("[madbfs] unmount with 'fusermount -u {:?}'", opt.fuse.mount);

    if (::setenv("ANDROID_SERIAL", opt.app.serial.c_str(), 1) < 0) {
        fmt::println(stderr, "error: failed to set env variable 'ANDROID_SERIAL' ({})", strerror(errno));
        return 1;
    }

    return madbfs::ll::run(opt);
}

int main(int argc, char** argv)
try {
    std::set_terminate(termination);

    auto context = async::Context{};
    auto parsed  = async::once(context, args::parse(argc, argv));

    context.restart();

    return parsed.visit(Overload{
        [&](args::MountOpt& opt) { return mount(opt); },
        [&](args::PushOpt& opt) { return async::once(context, push(opt)); },
        [](int ret) { return ret; },
    });
} catch (const std::exception& e) {
    fmt::println(stderr, "error: exception occurred: {}", e.what());
    return 1;
} catch (...) {
    fmt::println(stderr, "error: exception occurred (unknown exception)");
    return 1;
}
