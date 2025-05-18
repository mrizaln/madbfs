#include "adbfsm/adbfsm.hpp"
#include "adbfsm/args.hpp"
#include "adbfsm/log.hpp"

#include <execinfo.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <csignal>
#include <cstdlib>

/**
 * @brief Handler for unexpected program end.
 *
 * This handler is based on Stack Overflow answer:
 * - https://stackoverflow.com/a/77336/16506263
 */
void unexpected_program_end(const char* msg, bool is_sigsegv)
{
    fmt::print("> Program reached an unexpected end: {}", msg);

    if (not is_sigsegv) {
        auto exception = std::current_exception();
        if (exception != nullptr) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                fmt::print("> Uncaught exception:\n{}\n", e.what());
            } catch (...) {
                fmt::print("> Uncaught exception (unknown type)\n");
            }
        }
    }

    auto array = std::array<void*, 10>{};
    auto size  = ::backtrace(array.data(), array.size());

    adbfsm::log_c({ "> Backtrace:" });
    auto names = ::backtrace_symbols(array.data(), size);
    for (auto i : adbfsm::sv::iota(0, size)) {
        adbfsm::log_c({ "\t{}" }, names[i]);
    }
    fmt::println("> Backtrace:");
    for (auto i : adbfsm::sv::iota(0, size)) {
        fmt::println("\t{}", names[i]);
    }
    free(names);

    if (is_sigsegv) {
        std::signal(SIGSEGV, SIG_DFL);
        std::raise(SIGSEGV);
    }
}

int main(int argc, char** argv)
{
    std::signal(SIGSEGV, [](int) { unexpected_program_end("SIGSEGV", true); });
    std::set_terminate([] { unexpected_program_end("std::terminate", false); });

    auto ctx = adbfsm::async::Context{};
    auto fut = adbfsm::async::spawn(ctx, adbfsm::args::parse(argc, argv), adbfsm::async::use_future);
    ctx.run();

    auto maybe_opt = fut.get();
    if (maybe_opt.is_exit()) {
        return std::move(maybe_opt).exit().status;
    }
    auto [opt, args] = std::move(maybe_opt).opt();

    adbfsm::log::init(opt.log_level, opt.log_file);
    adbfsm::log_i(
        { "mount device '{}' with cache size {}MiB and page size {}kiB" },
        opt.serial,
        opt.cachesize,
        opt.pagesize
    );
    if (opt.log_file != "-") {
        fmt::println(
            "[adbfsm] mount device '{}' with cache size {}MiB and page size {}kiB",
            opt.serial,
            opt.cachesize,
            opt.pagesize
        );
    }

    if (::setenv("ANDROID_SERIAL", opt.serial.c_str(), 1) < 0) {
        fmt::println(stderr, "error: failed to set env variable 'ANDROID_SERIAL' ({})", strerror(errno));
        return 1;
    }

    auto ret = fuse_main(args.argc, args.argv, &adbfsm::operations, (void*)&opt);
    ::fuse_opt_free_args(&args);

    // on invalid argument (1) and no mount point specified (2)
    if (ret == 1 or ret == 2) {
        adbfsm::args::show_help(argv[0], true);
    }

    return ret;
}
