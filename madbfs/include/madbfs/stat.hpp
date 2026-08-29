#pragma once

#include <madbfs-common/aliases.hpp>

#include <sys/types.h>

namespace madbfs
{
    /**
     * @class Id
     *
     * @brief Strong type for identifying a node entry in `Filesystem`.
     */
    struct Id
    {
        u32 index;
        u32 version;

        constexpr static Id from_u64(u64 v)
        {
            return {
                .index   = static_cast<u32>(v),
                .version = static_cast<u32>(v >> 32),
            };
        }

        constexpr u64 to_u64() const
        {
            return static_cast<u64>(version) << 32 | index;    //
        }

        constexpr bool operator<=>(const Id&) const = default;
    };

    /**
     * @class Stat
     *
     * @brief File status information.
     *
     * This is a simplified `struct stat` (see man(3) stat). On creation, id shouldn't be set manually.
     */
    struct Stat
    {
        nlink_t  links = 1;
        off_t    size  = 0;
        timespec mtime = {};    // last modification time (only seconds part is used)
        timespec atime = {};
        timespec ctime = {};
        mode_t   mode  = 0;    // -rwxrwxrwx
        uid_t    uid   = 0;
        gid_t    gid   = 0;
    };

    /**
     * @brief Detect modification in remote.
     *
     * @param host Host side of stat.
     * @param remote Remote side of stat.
     *
     * This function only considers a modification in the remote is made when the remote stat is newer than
     * the host.
     *
     * TODO: Maybe store inode on the Stat then use it to detect modification? At least for files
     */
    inline bool detect_modification(const Stat& host, const Stat& remote)
    {
        // The modification detection only considers the modification time of the file in seconds
        // resolution. Furthermore this funciton tolerates 2 seconds of difference since the FileTree
        // can't always tracks the same timestamp of the files stored in the cache and the remote.

        static constexpr auto tolerance_sec = 2;
        return remote.mtime.tv_sec - host.mtime.tv_sec > tolerance_sec;
    }

    /**
     * @brief File open mode.
     */
    enum class OpenMode : u8
    {
        Read      = 0,
        Write     = 1,
        ReadWrite = 2,
    };

    enum class RenameMode : u8
    {
        Normal    = 0,
        Noreplace = 1 << 0,
        Exchange  = 1 << 1,
    };
}
