#pragma once

#include "madbfs/data/stat.hpp"
#include "madbfs/path.hpp"

#include <madbfs-common/async/async.hpp>

#include <saf.hpp>

#include <cassert>
#include <list>
#include <map>
#include <unordered_map>

namespace madbfs::connection
{
    class Connection;
}

namespace madbfs::data
{
    /**
     * @class PageKey
     *
     * @brief Key used to index page of a file in the LRU cache.
     */
    struct PageKey
    {
        Id    id;
        usize index;
        bool  operator==(const PageKey& other) const = default;
    };

    /**
     * @class Page
     *
     * @brief Represent a chunk of file content.
     */
    class Page
    {
    public:
        Page(PageKey key, Uniq<char[]> buf, u32 size, u32 page_size);

        usize read(Span<char> out, usize offset);
        usize write(Span<const char> in, usize offset);
        usize truncate(usize size);

        usize size() const;

        bool is_dirty() const;
        void set_dirty(bool set);

        const PageKey&   key() { return m_key; }
        Span<const char> buf() { return { m_data.get(), size() }; }

    private:
        PageKey      m_key;
        Uniq<char[]> m_data;
        u32          m_size;
        u32          m_page_size;
        bool         m_dirty = false;
    };

    /**
     * @class Cache
     *
     * @brief Manage file content cache using LRU as its approach.
     *
     * The cache is implemented as an LRU cache in order to speed up repeated access to recently accessed
     * files. Each element in the LRU is a `Page` that represents a portion of a file being stored. This
     * pages are interleaved between files (cross-file).
     *
     * The class also acts as debouncer for read/write operations.
     */
    class Cache
    {
    public:
        struct LookupEntry;

        using Lru    = std::list<Page>;
        using Lookup = std::unordered_map<Id, LookupEntry>;
        using Queue  = std::unordered_map<PageKey, saf::shared_future<Errc>>;

        /**
         * @class LookupEntry
         *
         * @brief Entry for lookup table.
         *
         * Contains information of a file being cached (including all the cached pages).
         */
        struct LookupEntry
        {
            std::map<usize, Lru::iterator> pages;
            bool                           dirty = false;
        };

        struct FdEntry
        {
            u64            fd;
            data::OpenMode mode;
        };

        /**
         * @brief Construct a new cache.
         *
         * @param connection Connection to device.
         * @param page_size Page size.
         * @param max_pages Maximum number of pages cached.
         */
        Cache(connection::Connection& connection, usize page_size, usize max_pages);

        /**
         * @brief Hint the cache to open a real fd to a file in the device for further operations.
         *
         * @param id Associated node.
         * @param path Associated path.
         * @param mode Access mode of the operation.
         *
         * This function will only open a real file if the file is not opened yet. If file is already opened
         * and the mode can upgraded from O_RDONLY or O_WRONLY to O_RDRW the file will be closed then reopened
         * with the O_RDRW mode.
         */
        AExpect<void> hint_open(Id id, path::Path path, data::OpenMode mode);

        /**
         * @brief Close the associated fd to real file for this node.
         *
         * @param id Associated node Id.
         *
         * Unlike `hint_open()` this function will immediately close the file descriptor to the real file.
         */
        AExpect<void> hint_close(Id id);

        /**
         * @brief Read bytes from file with desired id at an offset into buffer.
         *
         * @param id File id.
         * @param out Output buffer.
         * @param offset Read offset.
         *
         * @return Number of read bytes.
         *
         * This function will attempt to fetch for new data from the device if it can't found an entry in the
         * LRU. It may also flush an entry (may or may not be related to the file being read) to make space
         * for new data if the LRU pages reaches its maximum.
         */
        AExpect<usize> read(Id id, Span<char> out, off_t offset);

        /**
         * @brief Write bytes into file with desired id at an offset from buffer.
         *
         * @param id File id.
         * @param in Input buffer.
         * @param offset Write offset.
         *
         * @return Numbef of written bytes.
         *
         * This function only writes into the cache in memory. Writes to the actual file on the device will
         * only happen when eviction occurs or `flush()` is explicitly called.
         */
        AExpect<usize> write(Id id, Span<const char> in, off_t offset);

        /**
         * @brief Flush data into actual file to the device.
         *
         * @param id File id.
         */
        AExpect<void> flush(Id id);

        /**
         * @brief Truncate a file in the cache.
         *
         * @param id File id.
         * @param old_size Old file size.
         * @param new_size New file size.
         *
         * Unlike `read()` and `write()` this function don't actually interact with the actual file on the
         * device. The actual truncation should be done by the caller before calling this function. It will
         * only truncate the cached file content if it actually exists in the LRU.
         *
         * TODO: do the truncation here, why separate it?
         */
        AExpect<void> truncate(Id id, usize old_size, usize new_size);

        /**
         * @brief Invalidate entries for a file by its id.
         *
         * @param id File id.
         * @param should_flush Controls whether to flush the buffer if dirty.
         */
        Await<void> invalidate_one(Id id, bool should_flush);

        /**
         * @brief Invalidate all entries.
         *
         * This function is equaivalent to `shutdown()` function.
         */
        Await<void> invalidate_all();

        /**
         * @brief Shut down the cache and invalidate all cache entries.
         *
         * This function is equaivalent to `invalidate_all()` function.
         */
        Await<void> shutdown();

        Await<void> set_page_size(usize new_page_size);
        Await<void> set_max_pages(usize new_max_pages);

        usize page_size() const { return m_page_size; }
        usize max_pages() const { return m_max_pages; }
        usize current_pages() const { return m_lru.size(); }

    private:
        /**
         * @brief Look up for pages using its file id.
         *
         * @param id File identifier.
         * @param add_entry If true add new entry if lookup failed.
         *
         * @return A lookup entry or none if not exists.
         *
         * The returned value will be `std::nullopt` only when lookup failed and `add_entry` is set to false.
         * In the case of lookup failure and `add_entry` is set to true, the newly created entry is returned
         * (zero page).
         */
        Opt<Ref<LookupEntry>> lookup(Id id, bool add_entry);

        /**
         * @brief Operation to do on cache miss.
         *
         * @param id File id.
         * @param out Output buffer.
         * @param offset Read offset.
         *
         * May be called on `read()` function call.
         */
        AExpect<usize> on_miss(Id id, Span<char> out, off_t offset);

        /**
         * @brief Operation to do on flush.
         *
         * @param id File id.
         * @param in Input buffer.
         * @param offset Write offset.
         *
         * May be called on `read()`, `write()`, `invalidate_one()`, `invalidate_all()`, and `shutdown()`.
         * Will be called on `flush()`.
         */
        AExpect<usize> on_flush(Id id, Span<const char> in, off_t offset);

        /**
         * @brief Evict last entries in the LRU.
         *
         * @param size Number of last entries to be evicted.
         */
        Await<void> evict(usize size);

        /**
         * @brief Read file at page index.
         *
         * @param entry Lookup entry for the associated file.
         * @param out Output buffer.
         * @param id File id.
         * @param index Page index of the operation.
         * @param first First index of the page.
         * @param last Last index of the page.
         * @param offset Offset of the read operation.
         *
         * The `first` and `last` paramaters are related to the `offset` and `out` buffer size.
         */
        AExpect<usize> read_at(
            LookupEntry& entry,
            Span<char>   out,
            Id           id,
            usize        index,
            usize        first,
            usize        last,
            off_t        offset
        );

        /**
         * @brief Write file at page index.
         *
         * @param entry Lookup entry for the associated file.
         * @param int Input buffer.
         * @param id File id.
         * @param index Page index of the operation.
         * @param first First index of the page.
         * @param last Last index of the page.
         * @param offset Offset of the read operation.
         *
         * The `first` and `last` paramaters are related to the `offset` and `in` buffer size.
         */
        AExpect<usize> write_at(
            LookupEntry&     entry,
            Span<const char> in,
            Id               id,
            usize            index,
            usize            first,
            usize            last,
            off_t            offset
        );

        /**
         * @brief Flush file at page index.
         *
         * @param page Page to be flushed.
         * @param id Associated file id of the page.
         */
        AExpect<void> flush_at(Page& page, Id id);

        connection::Connection&         m_connection;
        std::unordered_map<Id, FdEntry> m_fd_map;    // maps node Id to real fd on to a file on device

        Lru    m_lru;      // most recently used is at the front
        Lookup m_table;    // lookup table for fast page access
        Queue  m_queue;    // pages that are still pulling data, reader/writer should wait using this

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
};
