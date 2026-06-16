#pragma once

#include "madbfs/cache/dirty_map.hpp"
#include "madbfs/stat.hpp"

namespace madbfs::cache
{
    class PageStore;

    /**
     * @class PageKey
     *
     * @brief Identify Page with actual file and its offset (in page_size units).
     */
    struct PageKey
    {
        Id    id;
        usize index;

        bool operator==(const PageKey&) const = default;
    };

    /**
     * @class PageId
     *
     * @brief Unique identifier for a Page in PageStore.
     */
    struct PageId
    {
        usize inner = 0;
    };

    /**
     * @class Page
     *
     * @brief Represent a chunk of file content.
     *
     * Every `Page` instance is managed by `PageStore`. Each page has metadata that are 1/8th of the size of
     * the page, so in effect, evey page takes up used up 9/8 of page size of memory. The metadata is used to
     * track which bytes are dirty/written which user can control via `set_dirty()`.
     */
    class Page
    {
    public:
        friend PageStore;

        Page() = default;

        /**
         * @brief Read the bytes from the page into output buffer.
         *
         * @param out Output buffer.
         * @param offset Read offset.
         *
         * @return Number of bytes written into the output buffer.
         */
        usize read(Span<char> out, usize offset);

        /**
         * @brief Write the bytes from input buffer into the page.
         *
         * @param in Input buffer.
         * @param offset Write offset.
         *
         * @return Number of bytes written into the page.
         */
        usize write(Span<const char> in, usize offset);

        /**
         * @brief Truncate the page content.
         *
         * @param size New size.
         *
         * @return Truncated size.
         *
         * If the new size is bigger than current size of the page, the gap will be filled with zeroes.
         */
        usize truncate(usize size);

        /**
         * @brief Return the entire span of the buffer of the page (page size).
         */
        Span<u8> buf();

        /**
         * @brief Check if the page has dirty regions.
         *
         * Use `iter_dirty()` to iterate over the dirty regions.
         */
        bool is_dirty() const { return m_dirty; }

        /**
         * @brief Check if the entire page is dirty.
         *
         * The entire region is dirty, you can use `buf()` for the entirety in this case.
         */
        bool is_fully_dirty() const { return m_fully_dirty; }

        /**
         * @brief Set the bytes specified from start to end as dirty.
         *
         * @param start Start byte index.
         * @param end End byte index.
         *
         * The range is exclusive: [start, end)
         */
        void set_dirty(usize start, usize end);

        /**
         * @brief Mark the entire page as dirty.
         */
        void set_fully_dirty();

        /**
         * @brief Mark the page as not dirty.
         */
        void clear_dirty();

        /**
         * @brief Iterate over dirty regions of the page.
         *
         * The dirty regions are provided as [start, end) pair. see `DirtyRange` and `DirtyIter`.
         */
        Range auto iter_dirty() const { return sv::all(m_dirty_map); }

        /**
         * @brief Set the size of page manually.
         *
         * @param size The desired size.
         *
         * This is useful if you plan to write into `buf()` manually to pass into a C interface for example.
         */
        void set_size(u32 size) { m_size = std::min(size, m_capacity); }

        /**
         * @brief The size of the page.
         *
         * While the capacity of the buffer of the page is page size, the size of the buffer that matters
         * is different. This value can be modified by `set_size()`, `write()`, or `truncate()`. It should
         * never exceed the page size.
         */
        usize size() const { return m_size; }

    private:
        /**
         * @brief Create a new Page.
         *
         * @param data Pointer to array of bytes.
         * @param page_size The size of the page.
         */
        Page(u8* data, usize page_size);

        u8*      m_data;
        DirtyMap m_dirty_map;

        u32 m_size     = 0;
        u32 m_capacity = 0;    // page size

        bool m_dirty       = false;
        bool m_fully_dirty = false;
    };

    /**
     * @class KeyedPage
     *
     * @brief Reference to page and its associated key.
     */
    struct KeyedPage
    {
        Page&   page;
        PageKey key;
    };

    /**
     * @class KeyedPageId
     *
     * @brief Unique identifier of a page and its associated key (if exist).
     */
    struct KeyedPageId
    {
        PageId       page_id;
        Opt<PageKey> key;
    };

    /**
     * @class PageStore
     *
     * @brief Page storage as well as an LRU.
     *
     * This is the Page storage for the cache. The Data storage itself is a contighous memory that got split
     * up by `page_size` (+ metadata) for `max_pages` times. The store acts like LRU that has limited amount
     * of available pages. The user can only store a `PageId` to the `Page` itself, while `PageStore` manages
     * the lifetime of the `Page`s. A page will be recycled if the user try to `acquire()` one. Thus It is the
     * responsibility of the user what to do with the returned `Page` when `acquire()` returned an already
     * used `Page` (key is useful for this case, you should track them somewhere).
     */
    class PageStore
    {
    public:
        /**
         * @brief Create a new storage of `Page`s with LRU style storage system.
         *
         * @param page_size The number of bytes a page have in bytes.
         * @param max_pages The number of pages the storage will have.
         */
        PageStore(usize page_size, usize max_pages);

        /**
         * @brief Acquire new page.
         *
         * @param key The key of the page.
         *
         * @return A struct than contains a unique identifier of the page and old key of the page if any.
         *
         * This operation will use the `Page` at the back of the LRU then moving it to to the front of the
         * LRU, effectively refreshing it.
         *
         * The `Page` returned may be an old `Page` that has data from previous usage (use the key to identify
         * it). If you want to zeroes the data, you need to do that manually (setting the size to zero, then
         * truncate to the desired size is one way to do it).
         */
        KeyedPageId acquire(Opt<PageKey> key);

        /**
         * @brief Get the actual `Page` instance from the associated unique id.
         *
         * @param id Unique identifier of a `Page`.
         * @param update_position Whether to move the `Page` to the front of the LRU.
         *
         * @return A struct containing a reference to the `Page` instance and its key.
         *
         * If a page that got `release()`-ed is the one associated with the `id`, the behavior is undefined
         * (assertion error on debug).
         */
        KeyedPage get(PageId id, bool update_position);

        /**
         * @brief Mark the `Page` that associated by its `PageId` as unused.
         *
         * @param id Unique identifier of a `Page`.
         *
         * The call to this function is basically a promise that you won't use the `Page` of the associated
         * `PageId` again for anything until the `PageId` is produced from `acquire()` again.
         */
        void release(PageId id);

        /**
         * @brief Mark all of the `Page` managed by this storage as unused.
         *
         * The same constraint apply as `release()` but applied to all pages produced by this instance.
         */
        void release_all();

        /**
         * @brief Count number of active `Page`s managed by this storage.
         *
         * An active page is a page that is not marked as unused.
         */
        usize count() const;

        usize page_size() const { return m_page_size; }
        usize max_pages() const { return m_max_pages; }

    private:
        struct PageNode
        {
            Page         page;
            Opt<PageKey> key;

            usize next;
            usize prev;
        };

        /**
         * @brief Move `Page` at specified index to front.
         *
         * @param index Index of page node in the array.
         */
        void to_front(usize index);

        /**
         * @brief Return the size of the stride of each page in the data storage (in bytes).
         */
        usize page_stride() const { return m_page_size + m_page_size / 8; }

        Vec<u8>       m_data;     // the bytes storage for the pages
        Vec<PageNode> m_pages;    // LRU

        usize m_front = 0;
        usize m_back  = 0;

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
}
