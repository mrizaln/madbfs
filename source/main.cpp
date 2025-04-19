#include "adbfsm/adbfsm.hpp"
#include "adbfsm/args.hpp"
#include "adbfsm/cmd.hpp"

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

    // remove the temporary directory
    if (auto* fuse_ctx = fuse_get_context(); fuse_ctx != nullptr) {
        adbfsm::log_c({ "> There is no fuse context! temp dir can't be deleted" });
        auto& data = *static_cast<adbfsm::AdbfsmData*>(fuse_ctx->private_data);
        std::filesystem::remove_all(data.m_dir);
    }

    if (is_sigsegv) {
        std::signal(SIGSEGV, SIG_DFL);
        std::raise(SIGSEGV);
    }
}

int main(int argc, char** argv)
{
    fmt::println("[adbfsm] checking adb availability...");
    if (auto serv = adbfsm::cmd::exec({ "adb", "start-server" }); serv.returncode != 0) {
        fmt::println(stderr, "error: failed to start adb server, make sure adb is installed and in PATH");
        fmt::println(stderr, "stderr:\n{}", serv.cerr);
        return 1;
    }

    std::signal(SIGSEGV, [](int) { unexpected_program_end("SIGSEGV", true); });
    std::set_terminate([] { unexpected_program_end("std::terminate", false); });

    auto maybe_opt = adbfsm::args::parse(argc, argv);
    if (maybe_opt.is_exit()) {
        return std::move(maybe_opt).exit().m_status;
    }
    auto [opt, args] = std::move(maybe_opt).opt();

    adbfsm::log::init(opt.m_log_level, opt.m_log_file);
    adbfsm::log_i({ "mount device '{}' with cache size {}MB" }, opt.m_serial, opt.m_cachesize);
    if (opt.m_log_file != "-") {
        fmt::println("[adbfsm] mount device '{}' with cache size {}MB", opt.m_serial, opt.m_cachesize);
    }

    if (::setenv("ANDROID_SERIAL", opt.m_serial.c_str(), 1) < 0) {
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
