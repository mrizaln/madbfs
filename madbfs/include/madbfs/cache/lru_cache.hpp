#pragma once

#include "madbfs/path.hpp"
#include "madbfs/stat.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>

namespace madbfs
{
    class Connection;
}

namespace madbfs::cache
{
    /**
     * @class LruCache
     *
     * @brief Manage file content cache using LRU as its approach.
     *
     * The cache is implemented as an LRU cache in order to speed up repeated access to recently accessed
     * files. Each element in the LRU is a `Page` that represents a portion of a file being stored. This
     * pages are interleaved between files (cross-file).
     *
     * This Cache stores a pair of real fds for read and write operations, even when the FUSE client opens
     * multiple files. Every file is discriminated by its id. Real fds are not exposed through this mode.
     *
     * The class also acts as debouncer for read/write operations because of the nature of it.
     */
    class LruCache
    {
    public:
        ~LruCache();

        LruCache(LruCache&&) noexcept;
        LruCache& operator=(LruCache&&) noexcept;

        LruCache(const LruCache&)            = delete;
        LruCache& operator=(const LruCache&) = delete;

        /**
         * @brief Create a new LRU-based cache.
         *
         * @param connection Conneciton to device.
         * @param page_size Cache page size.
         * @param max_pages Number of maximum pages the cache can hold.
         *
         * The connection will be held by the instance until it is destroyed.
         */
        static Await<LruCache> create(Connection& connection, usize page_size, usize max_pages);

        Await<void> initialize();

        Await<void> shutdown();

        /**
         * @brief Hint the cache to open a real fd to a file in the device for further operations.
         *
         * @param id Associated node.
         * @param path Associated path.
         * @param mode Access mode of the operation.
         *
         * This function will only open a real file if the file is not opened yet.
         * */
        AExpect<void> hint_open(Id id, path::Path path, OpenMode mode);

        /**
         * @brief Close the associated fd to real file for this node.
         *
         * @param id Associated node Id.
         * @param mode The open mode of the file.
         *
         * This function will close the the real file if the number of `hint_close()` is the same as
         * `hint_open()`. If both is matched, then the associated fd will be marked as stale. Call
         * `clean_stale_fds()` to actually close them.
         */
        AExpect<void> hint_close(Id id, OpenMode mode);

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
         * for new data if the LRU pages reache its maximum.
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
         * @brief Rename/relink path pointed by its id to a new path.
         *
         * @param id File id.
         * @param new_name New path for the file.
         *
         * This function only renames path in the cumulative fd map, not the real device.
         */
        Await<void> rename(Id id, path::Path new_name);

        /**
         * @brief Invalidate entries for a file by its id.
         *
         * @param id File id.
         * @param should_flush Controls whether to flush the buffer if dirty.
         */
        Await<void> invalidate_one(Id id, bool should_flush);

        /**
         * @brief Invalidate all entries.
         */
        Await<void> invalidate_all();

        /**
         * @brief Remove unused fds.
         */
        Await<void> clean_stale_fds();

        /**
         * @brief Remove all fds.
         *
         * @param close Whether to close the fds on remove.
         *
         * If you already know that the connection to the device is not available, you can set the `close`
         * parameter to `false` to not bother trying closing the fds from the device.
         */
        Await<void> invalidate_fds(bool close);

        /**
         * @brief Set new page size for the cache.
         *
         * @param new_page_size Non-zero integer; power of 2.
         *
         * Setting a new page size will invalidate cache entries. Dirty pages will be flushed as well.
         */
        Await<void> set_page_size(usize new_page_size);

        /**
         * @brief Set new maximum number of pages the cache able to hold.
         *
         * @param new_page_size Non-zero integer.
         *
         * Setting a new max pages will invalidate cache entries. Dirty pages will be flushed as well.
         */
        Await<void> set_max_pages(usize new_max_pages);

        /**
         * @brief Get the cache page size.
         */
        usize page_size() const;

        /**
         * @brief Get the cache max pages.
         */
        usize max_pages() const;

        /**
         * @brief Get current number of pages held by cache.
         */
        usize current_pages() const;

    private:
        class Impl;

        LruCache(Uniq<Impl> impl);

        Uniq<Impl> m_impl;
    };

    static_assert(std::move_constructible<LruCache>);
}
