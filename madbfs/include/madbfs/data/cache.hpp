#pragma once

#include "madbfs/data/stat.hpp"
#include "madbfs/path/path.hpp"

#include <madbfs-common/async/async.hpp>

#include <ankerl/unordered_dense.h>
#include <saf.hpp>

#include <cassert>
#include <list>

namespace madbfs::connection
{
    class Connection;
}

namespace madbfs::path
{
    class Path;
    class PathBuf;
}

namespace madbfs::data
{
    struct PageKey
    {
        Id    id;
        usize index;
        bool  operator==(const PageKey& other) const = default;
    };

    class Page
    {
    public:
        Page(PageKey key, Uniq<char[]> buf, u32 size, u32 page_size);

        usize read(Span<char> out, usize offset);
        usize write(Span<const char> in, usize offset);

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
        bool         m_dirty;
    };

    class Cache
    {
    public:
        struct PathEntry;

        using Lru     = std::list<Page>;
        using Lookup  = ankerl::unordered_dense::map<PageKey, Lru::iterator>;
        using Queue   = ankerl::unordered_dense::map<PageKey, saf::shared_future<Errc>>;
        using PathMap = ankerl::unordered_dense::map<Id, PathEntry>;

        struct PathEntry
        {
            usize         count;
            path::PathBuf path;
        };

        Cache(connection::Connection& connection, usize page_size, usize max_pages);

        AExpect<usize> read(Id id, path::Path path, Span<char> out, off_t offset);
        AExpect<usize> write(Id id, path::Path path, Span<const char> in, off_t offset);
        AExpect<void>  flush(Id id, usize size);

        void invalidate();
        void set_page_size(usize new_page_size);
        void set_max_pages(usize new_max_pages);

        usize page_size() const { return m_page_size; }
        usize max_pages() const { return m_max_pages; }

    private:
        AExpect<usize> on_miss(Id id, Span<char> out, off_t offset);
        AExpect<usize> on_flush(Id id, Span<const char> in, off_t offset);

        Await<void> evict(usize size);

        connection::Connection& m_connection;

        Lru     m_lru;      // most recently used is at the front
        Lookup  m_table;    // lookup table for fast page access
        Queue   m_queue;    // pages that are still pulling data, reader/writer should wait using this
        PathMap m_path_map;

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
};
