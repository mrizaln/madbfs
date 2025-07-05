#pragma once

#include <madbfs-common/aliases.hpp>

#include <sys/types.h>

#include <atomic>

namespace madbfs::data
{
    /**
     * @class CacheId
     *
     * @brief Strong type for identifying an entry in the cache.
     */
    class Id
    {
    public:
        friend struct Stat;

        Id() = default;
        u64 inner() const { return m_inner; }

        auto operator<=>(const Id&) const = default;

    private:
        inline static std::atomic<u64> s_id_counter = 0;

        static Id incr() { return { s_id_counter.fetch_add(1, std::memory_order::relaxed) + 1 }; }

        Id(u64 inner)
            : m_inner{ inner }
        {
        }

        u64 m_inner = 0;
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
        Id       id    = Id::incr();
        nlink_t  links = 1;
        off_t    size  = 0;
        timespec mtime = {};    // last modification time (only seconds part is used)
        timespec atime = {};
        timespec ctime = {};
        mode_t   mode  = 0;    // -rwxrwxrwx
        uid_t    uid   = 0;
        gid_t    gid   = 0;
    };
}
