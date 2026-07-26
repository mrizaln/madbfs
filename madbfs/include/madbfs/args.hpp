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
     * @class FuseArgs
     *
     * @brief RAII fuse_args cause I don't want to deal with manual deallocation.
     */
    class FuseArgs
    {
    public:
        FuseArgs(const FuseArgs&)            = delete;
        FuseArgs& operator=(const FuseArgs&) = delete;

        FuseArgs()
            : args{}
        {
        }

        FuseArgs(int argc, char** argv) { args = FUSE_ARGS_INIT(argc, argv); }

        FuseArgs(FuseArgs&& other)
            : args{ std::exchange(other.args, {}) }
        {
        }

        FuseArgs& operator=(FuseArgs&& other)
        {
            if (&other != this) {
                ::fuse_opt_free_args(&args);
                args = { std::exchange(other.args, {}) };
            }
            return *this;
        }

        ~FuseArgs() { ::fuse_opt_free_args(&args); }

        fuse_args&       inner() { return args; }
        const fuse_args& inner() const { return args; }

    private:
        fuse_args args;
    };

    // Connection strategy
    // -------------------
    struct AdbOnly
    {
    };

    struct NoServer
    {
        u16 port;
    };

    struct Server
    {
        adb::Abi abi;
        u16      port;
    };

    /**
     * @class Connection
     *
     * @brief Connection strategy (transport) to be used by the filesystem.
     */
    struct Connection : util::VarWrapper<AdbOnly, NoServer, Server>
    {
        using VarWrapper::VarWrapper;
    };
    // -------------------

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

    // Program options
    // ---------------
    struct MountOpt
    {
        FuseArgs      args;
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

    struct PushOpt
    {
        String serial;
    };

    /**
     * @class ParseResult
     *
     * @brief Parsed madbfs arguments.
     *
     * The integer variant is for when parsing failed; it contains a return code.
     */
    struct ParseResult : util::VarWrapper<MountOpt, PushOpt, int>
    {
        using VarWrapper::VarWrapper;
    };
    // ---------------

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
