#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

#include <bit>
#include <cassert>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace adbfsm::data
{
    using PageIndex = usize;

    // page size is not stored to minimize the memory usage
    class Page
    {
    public:
        Page(usize page_size, Span<const std::byte> data, usize offset, bool set_dirty)
            : m_data{ std::make_unique<std::byte[]>(page_size) }
            , m_size{ 0 }
        {
            std::copy_n(data.data(), data.size(), m_data.get() + offset);

            m_size = offset + data.size();
            dirty.store(set_dirty, std::memory_order::release);
        }

        Page(Page&& other)
            : m_data{ std::move(other.m_data) }
            , m_size{ other.m_size }
        {
            dirty.store(other.dirty.load(std::memory_order::acquire), std::memory_order::release);
        }

        usize size() const { return m_size; }

        template <typename T = std::byte>
            requires (sizeof(T) == 1)
        Span<const T> data_as() const
        {
            return Span{ reinterpret_cast<const T*>(m_data.get()), size() };
        }

        void write(Span<const std::byte> data, usize offset)
        {
            // kinda risky since there is no offset and data size check with the page size
            std::copy_n(data.data(), data.size(), m_data.get() + offset);
            m_size = offset + data.size();
        }

        std::atomic<bool> dirty = false;

    private:
        Uniq<std::byte[]> m_data;
        usize             m_size;    // will always less than or equal to page_size
    };

    class Cache
    {
    public:
        struct PageKey;
        struct MapEntry;

        using Lru   = std::list<PageKey>;
        using Pages = std::unordered_map<PageKey, MapEntry>;
        using Ord   = std::memory_order;

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

        Cache(usize page_size, usize max_pages)
            : m_page_size{ std::bit_ceil(page_size) }
            , m_max_pages{ max_pages }
        {
        }

        template <std::invocable<Span<std::byte>, off_t> OnMiss>
            requires std::same_as<std::invoke_result_t<OnMiss, Span<std::byte>, off_t>, Expect<usize>>
        Expect<usize> read(Id id, Span<char> out, off_t offset, OnMiss on_miss)
        {
            auto start = static_cast<usize>(offset) / m_page_size;
            auto last  = (static_cast<usize>(offset) + out.size() - 1) / m_page_size;

            auto total_read = 0uz;

            for (auto index : sv::iota(start, last + 1)) {
                auto read_lock = std::shared_lock{ m_mutex };

                auto key   = PageKey{ id, index };
                auto entry = m_pages.find(key);

                if (entry == m_pages.end()) {
                    auto data = Vec<std::byte>(m_page_size);
                    auto res  = on_miss(data, static_cast<off_t>(index * m_page_size));
                    if (not res.has_value()) {
                        return Unexpect{ res.error() };
                    }

                    read_lock.unlock();
                    auto write_lock = std::unique_lock{ m_mutex };

                    m_lru.emplace_front(key);

                    auto new_entry = MapEntry{
                        .page   = Page{ m_page_size, { data.begin(), res.value() }, 0, false },
                        .lru_it = m_lru.begin(),
                    };
                    auto [p, _] = m_pages.emplace(key, std::move(new_entry));
                    entry       = p;

                    if (m_pages.size() > m_max_pages) {
                        auto delta = m_pages.size() - m_max_pages;
                        while (delta-- > 0) {
                            auto last = m_lru.back();
                            m_lru.pop_back();
                            auto page = m_pages.extract(last);
                            if (not page.empty() and page.mapped().page.dirty.load(Ord::acquire)) {
                                m_orphan_pages.emplace_back(last, std::move(page.mapped().page));
                            }
                        }
                    }

                    write_lock.unlock();
                    read_lock.lock();
                }

                auto data = entry->second.page.data_as<char>();

                auto local_offset = 0uz;
                if (index == start) {
                    local_offset = static_cast<usize>(offset) % m_page_size;
                }

                auto read = std::min(data.size() - local_offset, out.size() - total_read);
                std::copy_n(data.data() + local_offset, read, out.data() + total_read);
                total_read += read;

                if (entry->second.lru_it != m_lru.begin()) {
                    read_lock.unlock();
                    auto write_lock = std::unique_lock{ m_mutex };

                    m_lru.erase(entry->second.lru_it);
                    m_lru.push_front(entry->first);
                    entry->second.lru_it = m_lru.begin();

                    write_lock.unlock();
                    read_lock.lock();
                }
            }

            return total_read;
        }

        Expect<usize> write(Id id, Span<const char> in, off_t offset)
        {
            auto start = static_cast<usize>(offset) / m_page_size;
            auto last  = (static_cast<usize>(offset) + in.size() - 1) / m_page_size;

            auto total_written = 0uz;

            for (auto index : sv::iota(start, last + 1)) {
                auto write_lock = std::unique_lock{ m_mutex };

                auto key  = PageKey{ id, index };
                auto page = m_pages.find(key);

                auto local_offset = 0uz;
                if (index == start) {
                    local_offset = static_cast<usize>(offset) % m_page_size;
                }

                auto write      = std::min(m_page_size - local_offset, in.size() - total_written);
                auto write_span = Span{
                    reinterpret_cast<const std::byte*>(in.data()) + total_written,
                    write,
                };

                if (page == m_pages.end()) {
                    m_lru.emplace_front(key);
                    auto new_entry = MapEntry{
                        .page   = Page{ m_page_size, write_span, local_offset, true },
                        .lru_it = m_lru.begin(),
                    };
                    m_pages.emplace(key, std::move(new_entry));
                } else {
                    page->second.page.write(write_span, local_offset);
                    page->second.page.dirty.store(true, Ord::release);

                    if (page->second.lru_it != m_lru.begin()) {
                        m_lru.erase(page->second.lru_it);
                        m_lru.push_front(page->first);
                        page->second.lru_it = m_lru.begin();
                    }
                }

                total_written += write;

                if (m_pages.size() > m_max_pages) {
                    auto delta = m_pages.size() - m_max_pages;
                    while (delta-- > 0) {
                        auto last = m_lru.back();
                        m_lru.pop_back();
                        auto page = m_pages.extract(last);
                        if (page.mapped().page.dirty.load(Ord::acquire)) {
                            m_orphan_pages.emplace_back(last, std::move(page.mapped().page));
                        }
                    }
                }
            }

            return total_written;
        }

        template <std::invocable<Span<const std::byte>, off_t> Fun>
            requires std::same_as<std::invoke_result_t<Fun, Span<const std::byte>, off_t>, Expect<usize>>
        Expect<void> flush(Id id, usize size, Fun on_flush)
        {
            auto num_pages = size / m_page_size + (size % m_page_size != 0);

            for (auto index : sv::iota(0uz, num_pages)) {
                auto read_lock = std::shared_lock{ m_mutex };    // should I use unique_lock here?

                auto key   = PageKey{ id, index };
                auto entry = m_pages.find(key);

                if (entry == m_pages.end()) {
                    continue;
                }

                auto& page = entry->second.page;
                if (page.dirty.load(Ord::acquire)) {

                    auto data = page.data_as<std::byte>();
                    auto res  = on_flush(data, static_cast<off_t>(index * m_page_size));
                    if (not res.has_value()) {
                        return Unexpect{ res.error() };
                    }

                    page.dirty.store(false, Ord::release);
                }
            }
            return {};
        }

        std::vector<Pair<PageKey, Page>> get_orphan_pages()
        {
            auto write_lock = std::unique_lock{ m_mutex };
            return std::move(m_orphan_pages);
        }

        bool has_orphan_pages() const
        {
            auto read_lock = std::shared_lock{ m_mutex };
            return not m_orphan_pages.empty();
        }

        usize page_size() const { return m_page_size; }

    private:
        mutable std::shared_mutex m_mutex;

        std::vector<Pair<PageKey, Page>> m_orphan_pages;    // dirty but evicted pages

        Pages m_pages;
        Lru   m_lru;    // most recently used is at the front

        usize m_page_size = 0;
        usize m_max_pages = 0;
    };
};
