#pragma once

#include "adbfsm/async/async.hpp"
#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

#include <functional>

#include <cassert>
#include <list>
#include <unordered_map>

namespace adbfsm::data
{
    struct PageKey
    {
        Id    id;
        usize index;
        bool  operator==(const PageKey& other) const = default;
    };

    // NOTE: page size is not stored to minimize the memory usage
    class Page
    {
    public:
        // NOTE: can't use std::move_only_function in gcc: "atomic constraint depends on itself"
        using WriteFn = std::function<Expect<Span<const char>>()>;

        Page(PageKey key, usize page_size);
        ~Page();

        usize read(Span<char> out, usize offset);
        usize write(Span<const char> in, usize offset);

        Expect<usize> write_fn(usize offset, WriteFn fn);

        usize size() const;

        bool is_dirty() const;
        void set_dirty(bool set);

        const PageKey& key() { return m_key; }

    private:
        static constexpr auto dirty_bit = 0x10000000_u32;

        PageKey      m_key;
        Uniq<char[]> m_data;
        u32          m_size;    // 1 bit is used as dirty flag, so max page size should be 2**31 bytes
    };

    class Cache
    {
    public:
        using Lru    = std::list<Page>;
        using Lookup = std::unordered_map<PageKey, Lru::iterator>;
        using Ord    = std::memory_order;

        // NOTE: can't use std::move_only_function in gcc: "atomic constraint depends on itself"
        using OnMiss  = std::function<AExpect<usize>(Span<char> out, off_t offset)>;
        using OnFlush = std::function<AExpect<usize>(Span<const char> in, off_t offset)>;

        Cache(usize page_size, usize max_pages);

        AExpect<usize> read(Id id, Span<char> out, off_t offset, OnMiss on_miss);
        AExpect<usize> write(Id id, Span<const char> in, off_t offset);
        AExpect<void>  flush(Id id, usize size, OnFlush on_flush);

        Cache::Lru get_orphan_pages();
        bool       has_orphan_pages() const;

        void invalidate();
        void set_page_size(usize new_page_size);
        void set_max_pages(usize new_max_pages);

        usize page_size() const { return m_page_size; }
        usize max_pages() const { return m_max_pages; }

    private:
        Lru    m_lru;    // most recently used is at the front
        Lookup m_table;

        Lru m_orphaned;    // dirty but evicted pages

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
};
