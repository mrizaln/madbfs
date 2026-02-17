#include "madbfs/args.hpp"
#include "madbfs/operations.hpp"

#include <madbfs-common/log.hpp>

#include <exception>
#include <print>

void termination()
{
    if (auto e = std::current_exception(); e != nullptr) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& e) {
            madbfs::log_c("> Uncaught exception:\n{}", e.what());
        } catch (...) {
            madbfs::log_c("> Uncaught exception (unknown type)");
        }
    }
    madbfs::log_c("> Terminating");
    madbfs::log::shutdown();
    std::abort();
}

int main(int argc, char** argv)
try {
    std::set_terminate(termination);

    auto parsed = madbfs::async::once(madbfs::args::parse(argc, argv));
    if (parsed.is_exit()) {
        return std::move(parsed).exit().status;
    }

    auto&& [opt, args] = std::move(parsed).opt();
    if (not madbfs::log::init(opt.log_level, opt.log_file)) {
        return 1;
    }

    std::println("[madbfs] mount '{}' [cache={} MiB, page={} KiB]", opt.serial, opt.cachesize, opt.pagesize);
    std::println("[madbfs] unmount with 'fusermount -u {:?}'", opt.mount);

    if (opt.log_file != "-") {
        madbfs::log_i(
            "[madbfs] mount '{}' at '{}' with cache size {} MiB and page size {} KiB",
            opt.serial,
            opt.mount,
            opt.cachesize,
            opt.pagesize
        );
    }

    if (::setenv("ANDROID_SERIAL", opt.serial.c_str(), 1) < 0) {
        std::println(stderr, "error: failed to set env variable 'ANDROID_SERIAL' ({})", strerror(errno));
        return 1;
    }

    auto ret = fuse_main(args.argc, args.argv, &madbfs::operations::operations, (void*)&opt);
    ::fuse_opt_free_args(&args);

    // on invalid argument (1) and no mount point specified (2)
    if (ret == 1 or ret == 2) {
        madbfs::args::show_help(argv[0]);
    }

    return ret;
} catch (const std::exception& e) {
    std::println(stderr, "error: exception occurred: {}", e.what());
    return 1;
} catch (...) {
    std::println(stderr, "error: exception occurred (unknown exception)");
    return 1;
}
