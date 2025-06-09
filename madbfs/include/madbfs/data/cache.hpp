#pragma once

#include "madbfs/data/stat.hpp"
#include "madbfs/path/path.hpp"

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

    class Cache
    {
    public:
        struct LookupEntry;

        using Lru    = std::list<Page>;
        using Lookup = std::unordered_map<Id, LookupEntry>;
        using Queue  = std::unordered_map<PageKey, saf::shared_future<Errc>>;

        struct LookupEntry
        {
            std::map<usize, Lru::iterator> pages;
            path::PathBuf                  path;
            bool                           dirty = false;
        };

        Cache(connection::Connection& connection, usize page_size, usize max_pages);

        AExpect<usize> read(Id id, path::Path path, Span<char> out, off_t offset);
        AExpect<usize> write(Id id, path::Path path, Span<const char> in, off_t offset);
        AExpect<void>  flush(Id id);
        AExpect<void>  truncate(Id id, usize old_size, usize new_size);

        Await<void> rename(Id id, path::Path new_name);
        Await<void> invalidate_one(Id id, bool should_flush);
        Await<void> invalidate_all();
        Await<void> shutdown();

        Await<void> set_page_size(usize new_page_size);
        Await<void> set_max_pages(usize new_max_pages);

        usize page_size() const { return m_page_size; }
        usize max_pages() const { return m_max_pages; }

    private:
        Opt<Ref<LookupEntry>> lookup(Id id, Opt<path::Path> path);

        AExpect<usize> on_miss(Id id, Span<char> out, off_t offset);
        AExpect<usize> on_flush(Id id, Span<const char> in, off_t offset);

        Await<void> evict(usize size);

        connection::Connection& m_connection;

        Lru    m_lru;      // most recently used is at the front
        Lookup m_table;    // lookup table for fast page access
        Queue  m_queue;    // pages that are still pulling data, reader/writer should wait using this

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
};
