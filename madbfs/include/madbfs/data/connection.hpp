#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs/data/stat.hpp"

namespace madbfs::path
{
    class Path;
    class PathBuf;
}

namespace madbfs::data
{
    struct ParsedStat
    {
        Stat stat;
        Str  path;
    };

    // NOTE: I made this interface to make Node and FileTree testable
    class Connection
    {
    public:
        virtual AExpect<Gen<ParsedStat>> statdir(path::Path path)  = 0;
        virtual AExpect<Stat>            stat(path::Path path)     = 0;
        virtual AExpect<path::PathBuf>   readlink(path::Path path) = 0;

        // directory operations
        virtual AExpect<void> mkdir(path::Path path)              = 0;
        virtual AExpect<void> rm(path::Path path, bool recursive) = 0;
        virtual AExpect<void> rmdir(path::Path path)              = 0;
        virtual AExpect<void> mv(path::Path from, path::Path to)  = 0;

        // file operations
        virtual AExpect<void>  truncate(path::Path path, off_t size)                     = 0;
        virtual AExpect<u64>   open(path::Path path, int flags)                          = 0;
        virtual AExpect<usize> read(path::Path path, Span<char> out, off_t offset)       = 0;
        virtual AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) = 0;
        virtual AExpect<void>  flush(path::Path path)                                    = 0;
        virtual AExpect<void>  release(path::Path path)                                  = 0;

        virtual AExpect<usize> copy_file_range(
            path::Path in,
            off_t      in_off,
            path::Path out,
            off_t      out_off,
            usize      size
        ) = 0;

        // directory operation (adding file) or file operation (update time)
        virtual AExpect<void> touch(path::Path path, bool create) = 0;

        virtual ~Connection() = default;
    };

    enum class AdbError
    {
        Unknown,
        NoDev,
        PermDenied,
        NoSuchFileOrDir,
        NotADir,
        Inaccessible,
        ReadOnly,
        TryAgain,
    };

    enum class DeviceStatus
    {
        Device,
        Offline,
        Unauthorized,
        Unknown,
    };

    struct Device
    {
        String       serial;
        DeviceStatus status;
    };

    /**
     * @brief Get human readable description of DeviceStatus.
     */
    Str to_string(DeviceStatus status);

    /**
     * @brief Execute a command.
     *
     * @param exe Executable name.
     * @param args Arguments to the command.
     * @param in Input data to be piped as stdin.
     * @param check Check return value.
     * @param merge_err Append stderr to stdout.
     */
    madbfs::Await<madbfs::Expect<madbfs::String>> exec_async(
        madbfs::Str                     exe,
        madbfs::Span<const madbfs::Str> args,
        madbfs::Str                     in,
        bool                            check,
        bool                            merge_err = false
    );

    /**
     * @brief Start connection with the devices.
     */
    AExpect<void> start_connection();

    /**
     * @brief List connected devices.
     */
    AExpect<Vec<Device>> list_devices();
}
