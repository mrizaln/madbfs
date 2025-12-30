#include "madbfs/args.hpp"
#include "madbfs/operations.hpp"

#include <madbfs-common/log.hpp>

#include <execinfo.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <print>

/**
 * @brief Handler for unexpected program end.
 *
 * This handler is based on Stack Overflow answer:
 * - https://stackoverflow.com/a/77336/16506263
 *
 * FIXME: Currently the signal handling is hacky and may not work properly because there are invocations to
 * functions that are not signal safe inside it.
 */
void unexpected_program_end(const char* msg, bool is_sigsegv)
{
    std::println("> Program reached an unexpected end: {}", msg);

    if (not is_sigsegv) {
        auto exception = std::current_exception();
        if (exception != nullptr) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                std::println("> Uncaught exception:\n{}", e.what());
            } catch (...) {
                std::println("> Uncaught exception (unknown type)");
            }
        }
    }

    auto array = std::array<void*, 10>{};
    auto size  = ::backtrace(array.data(), array.size());

    madbfs::log_c("> Backtrace:");
    auto names = ::backtrace_symbols(array.data(), size);
    for (auto i : madbfs::sv::iota(0, size)) {
        madbfs::log_c("\t{}", names[i]);
    }
    std::println("> Backtrace:");
    for (auto i : madbfs::sv::iota(0, size)) {
        std::println("\t{}", names[i]);
    }
    ::free(names);

    if (is_sigsegv) {
        std::signal(SIGSEGV, SIG_DFL);
        std::raise(SIGSEGV);
    }

    madbfs::log::shutdown();
}

int main(int argc, char** argv)
try {
    std::set_terminate([] { unexpected_program_end("std::terminate", false); });
    std::signal(SIGSEGV, [](int) { unexpected_program_end("SIGSEGV", true); });
    std::signal(SIGTERM, [](int) { madbfs::log::shutdown(); });

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
