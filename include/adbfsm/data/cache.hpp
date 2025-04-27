#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/path/path.hpp"

#include <atomic>
#include <deque>
#include <filesystem>

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
        virtual const Entry*         get(Id id) const                              = 0;
        virtual Expect<const Entry*> add(IConnection& connection, path::Path path) = 0;
        virtual bool                 remove(Id id)                                 = 0;

    protected:
        ~ICache() = default;
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
        Cache(std::filesystem::path path, usize max_size)
            : m_cache_dir{ std::move(path) }
            , m_max_size{ max_size }
        {
        }

        /**
         * @brief Get entry by its id.
         *
         * @param id The id of the entry
         *
         * @return Pointer to entry if exists, or nullptr if not exists.
         *
         * The returned pointer only valid until Cache::add is called. In the case of the entry not exist, it
         * may have been invalidated on Cache::add call. You may want to re-add the entry if that is the case.
         */
        const Entry* get(Id id) const override;

        /**
         * @brief Add a new entry into the cache
         *
         * @param connection IConnection interface to the device.
         * @param path Path to be inserted into the entry
         *
         * @return Pointer to Entry if successful or error if not successful.
         *
         * The returned pointer is guaranteed to be non-null.
         */
        Expect<const Entry*> add(IConnection& connection, path::Path path) override;

        /**
         * @brief Remove cache entry.
         *
         * @param id The id to the cache.
         *
         * @return True if the entry is removed, false if it's not exists.
         */
        bool remove(Id id) override;

    private:
        // files that are pulled from the device and associated with node in FileTree
        std::deque<Entry>     m_entries;
        std::filesystem::path m_cache_dir;

        usize m_max_size;
        usize m_current_size = 0;
    };
}
