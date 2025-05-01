#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/path/path.hpp"

#include <atomic>
#include <deque>
#include <shared_mutex>

namespace adbfsm::data
{
    class IConnection;

    /**
     * @class CacheId
     *
     * @brief Strong type for identifying an entry in the cache.
     */
    class Id
    {
    public:
        friend class Cache;

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

    struct Entry
    {
        Id            id;
        usize         size;
        Timestamp     time;
        path::PathBuf path;

        auto operator<=>(const Entry& other) const { return time <=> other.time; }
    };

    // NOTE: I made this interface to make Node and FileTree testable
    class ICache
    {
    public:
        virtual Opt<path::PathBuf> get(Id id) const             = 0;
        virtual bool               exists(Id id) const          = 0;
        virtual bool               set_dirty(Id id, bool dirty) = 0;

        virtual Expect<Id>   add(IConnection& connection, path::Path path) = 0;
        virtual Expect<bool> remove(IConnection& connection, Id id)        = 0;

        virtual Expect<bool> sync(IConnection& connection)         = 0;
        virtual Expect<bool> flush(IConnection& connection, Id id) = 0;

        virtual ~ICache() = default;
    };

    /**
     * @class Cache
     *
     * @brief Responsible for caching the files from and to the device.
     */
    class Cache : public ICache
    {
    public:
        // size in bytes
        Cache(path::Path path, usize max_size)
            : m_cache_dir{ path.into_buf() }
            , m_max_size{ max_size }
        {
        }

        /**
         * @brief Get entry by its id.
         *
         * @param id The id of the entry
         *
         * @return Path to cache file or nullopt if id not in entry.
         *
         * The returned path only valid until Cache::add is called. In the case of the entry not exist, it may
         * have been invalidated on Cache::add call. You may want to re-add the entry if that is the case.
         */
        Opt<path::PathBuf> get(Id id) const override;

        /**
         * @brief Check whether a cache is exists by its id.
         *
         * @param id The id of the entry
         */
        bool exists(Id id) const override;

        /**
         * @brief Set entry as dirty or modified.
         *
         * @param id Id of the entry.
         * @param dirty The dirty state of the entry.
         *
         * @return True if successfully changed the status of the entry.
         */
        bool set_dirty(Id id, bool dirty) override;

        /**
         * @brief Add a new entry into the cache
         *
         * @param connection IConnection interface to the device.
         * @param path Path to be inserted into the entry
         *
         * @return Id of the entry or error if not successful.
         *
         * The returned pointer is guaranteed to be non-null.
         */
        Expect<Id> add(IConnection& connection, path::Path path) override;

        /**
         * @brief Remove cache entry.
         *
         * @param connection IConnection interface to the device.
         * @param id The id to the cache.
         *
         * @return True if the entry is removed, false if it's not exists.
         */
        Expect<bool> remove(IConnection& connection, Id id) override;

        /**
         * @brief Sync the cache with the device.
         *
         * @param connection IConnection interface to the device.
         *
         * @return True if sync performed else false if there's nothing to sync.
         */
        Expect<bool> sync(IConnection& connection) override;

        /**
         * @brief Sync the cache with the device.
         *
         * @param connection IConnection interface to the device.
         * @param id The id to the cache.
         *
         * @return True if flush performed else false if there's nothing to flush.
         */
        Expect<bool> flush(IConnection& connection, Id id) override;

    private:
        mutable std::shared_mutex m_mutex;

        std::deque<Entry> m_entries;
        std::deque<Entry> m_entries_dirty;

        path::PathBuf m_cache_dir;

        usize m_max_size;
        usize m_current_size = 0;
    };
}
