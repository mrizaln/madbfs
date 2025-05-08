#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

#include <functional>
#include <shared_futex/shared_futex.hpp>

#include <cassert>
#include <list>
#include <unordered_map>

namespace adbfsm::data
{
    using PageIndex = usize;

    // NOTE: page size is not stored to minimize the memory usage
    class Page
    {
    public:
        // NOTE: can't use std::move_only_function in gcc: "atomic constraint depends on itself"
        using WriteFn = std::function<Expect<Span<const char>>()>;

        Page(usize page_size);
        Page(Page&& other);

        usize read(Span<char> out, usize offset);
        usize write(Span<const char> in, usize offset);

        Expect<usize> write_fn(usize offset, WriteFn fn);

        usize size() const;

        bool is_dirty() const;
        void set_dirty(bool set);

    private:
        // move ctor proxy
        Page(Page&& other, strt::exclusive_lock<strt::shared_futex_micro>);

        static constexpr auto dirty_bit = 0x10000000_u32;

        Uniq<char[]> m_data;
        u32          m_size;    // 1 bit is used as dirty flag, so max page size should be 2**31 bytes

        mutable strt::shared_futex_micro m_mutex;
    };

    class Cache
    {
    public:
        struct PageKey;
        struct MapEntry;

        using Lru   = std::list<PageKey>;
        using Pages = std::unordered_map<PageKey, MapEntry>;
        using Ord   = std::memory_order;

        // NOTE: can't use std::move_only_function in gcc: "atomic constraint depends on itself"
        using OnMiss  = std::function<Expect<usize>(Span<char> out, off_t offset)>;
        using OnFlush = std::function<Expect<usize>(Span<const char> in, off_t offset)>;

        struct PageKey
        {
            Id        id;
            PageIndex page_index;
            bool      operator==(const PageKey& other) const = default;
        };

        struct MapEntry
        {
            Page          page;
            Lru::iterator lru_it;    // since it's a list, this always valid
        };

        Cache(usize page_size, usize max_pages);

        Expect<usize> read(Id id, Span<char> out, off_t offset, OnMiss on_miss);
        Expect<usize> write(Id id, Span<const char> in, off_t offset);
        Expect<void>  flush(Id id, usize size, OnFlush on_flush);

        Vec<Pair<PageKey, Page>> get_orphan_pages();
        bool                     has_orphan_pages() const;

        usize page_size() const { return m_page_size; }

    private:
        mutable strt::shared_futex m_mutex;

        Vec<Pair<PageKey, Page>> m_orphan_pages;    // dirty but evicted pages

        Pages m_pages;
        Lru   m_lru;    // most recently used is at the front

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
};
