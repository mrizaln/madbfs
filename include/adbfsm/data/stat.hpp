#pragma once

#include "adbfsm/common.hpp"

#include <sys/stat.h>

#include <atomic>

namespace adbfsm::data
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

    struct Stat
    {
        Id      id    = Id::incr();
        nlink_t links = 1;
        off_t   size  = 0;
        time_t  mtime = {};    // last modification time (only seconds part is used)
        mode_t  mode  = 0;     // -rwxrwxrwx
        uid_t   uid   = 0;
        gid_t   gid   = 0;
    };
}
