#pragma once

#include "madbfs/adb.hpp"
#include "madbfs/path.hpp"

#include <madbfs-common/async/async.hpp>
#include <madbfs-common/log.hpp>
#include <madbfs-common/util/var_wrapper.hpp>

#define FUSE_USE_VERSION 31
#include <fuse_lowlevel.h>
#include <fuse_opt.h>

namespace madbfs::args
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

        ~MadbfsOpt()
        {
            ::free((void*)serial);
            ::free((void*)root);
            ::free((void*)log_level);
            ::free((void*)log_file);
        }
    };

    namespace connection
    {
        // clang-format off
        struct AdbOnly { };
        struct NoServer{ u16 port; };
        struct Server  { adb::Abi abi; u16 port; };
        // clang-format on
    };

    /**
     * @class Connection
     *
     * @brief Connection strategy (transport) to be used by the filesystem.
     */
    struct Connection : util::VarWrapper<connection::AdbOnly, connection::NoServer, connection::Server>
    {
        using VarWrapper::VarWrapper;
    };

    /**
     * @class Caching
     *
     * @brief User-defined cache parameters.
     */
    struct Caching
    {
        usize cachesize;
        usize pagesize;
    };

    /**
     * @class ParsedOpt
     *
     * @brief Parsed madbfs options.
     */
    struct ParsedOpt
    {
        String        mount;
        String        serial;
        path::PathBuf root;
        Connection    connection;
        Opt<Caching>  caching;
        log::Level    log_level;
        String        log_file;
        i32           ttl;
        i32           timeout;
    };

    /**
     * @class ParseResult
     *
     * @brief Argument parsing result.
     *
     * A variant between the parsed opt and exit code.
     */
    struct ParseResult
    {
        // clang-format off
        struct Opt  { ParsedOpt opt; fuse_args args; };
        struct Exit { int status; };

        ParseResult() : result{ Exit{ 0 } } {}
        ParseResult(Opt opt)    : result{ std::move(opt) } {}
        ParseResult(int status) : result{ Exit{ status } } {}

        bool is_opt()  const { return std::holds_alternative<Opt>(result);  }
        bool is_exit() const { return std::holds_alternative<Exit>(result); }

        Opt&&  opt()  && { return std::move(std::get<Opt>(result));  }
        Exit&& exit() && { return std::move(std::get<Exit>(result)); }

        Var<Opt, Exit> result;
        // clang-format on
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
        // clang-format on
        FUSE_OPT_END,
    });

    /**
     * @brief Print help into stdout.
     *
     * @param prog Program name.
     */
    void show_help(const char* prog);

    /**
     * @brief Parse the command line arguments; show help message if needed.
     *
     * @param argc Number of arguments.
     * @param argv Array of arguments.
     *
     * @return Result of the parsing operation.
     *
     * If the return value is `ParseResult::Opt`, the `args` member must be freed using `fuse_opt_free_args()`
     * after use.
     */
    Await<ParseResult> parse(int argc, char** argv);
}
