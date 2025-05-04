#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

#include <cassert>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace adbfsm::data
{
    using PageIndex = usize;

    static constexpr usize g_page_size      = 64 * 1024;            // 64 KiB
    static constexpr usize g_max_page_cache = 500 * 1024 * 1024;    // 500 MiB

    class Page
    {
    public:
        Page(Span<const std::byte> data, usize offset, bool dirty)
            : m_data{ std::make_unique<std::byte[]>(g_page_size) }
            , m_size{ 0 }
        {
            assert(offset < g_page_size);
            assert(data.size() <= g_page_size - offset);
            std::copy_n(data.data(), data.size(), m_data.get() + offset);

            m_size = offset + data.size();
            set_dirty(dirty);
        }

        Page(Page&& other)
            : m_data{ std::move(other.m_data) }
            , m_size{ other.m_size }
            , m_dirty{ other.m_dirty.load(std::memory_order::acquire) }
        {
        }

        usize size() const { return m_size; }

        bool is_dirty() { return m_dirty.load(std::memory_order::acquire); }
        void set_dirty(bool dirty) { m_dirty.store(dirty, std::memory_order::release); }

        template <typename T = std::byte>
            requires (sizeof(T) == 1)
        Span<const T> data_as() const
        {
            return Span{ reinterpret_cast<const T*>(m_data.get()), size() };
        }

        usize write(Span<const std::byte> data, usize offset)
        {
            auto copied = std::min(data.size(), g_page_size - offset);
            std::copy_n(data.data(), copied, m_data.get() + offset);
            m_size = offset + copied;
            return copied;
        }

    private:
        Uniq<std::byte[]> m_data;
        usize             m_size;    // will always less than or equal to page_size
        std::atomic<bool> m_dirty = false;
    };

    class Cache
    {
    public:
        struct PageKey;
        struct MapEntry;

        using Lru   = std::list<PageKey>;
        using Pages = std::unordered_map<PageKey, MapEntry>;

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

        template <std::invocable<Span<std::byte>, off_t> OnMiss>
            requires std::same_as<std::invoke_result_t<OnMiss, Span<std::byte>, off_t>, Expect<usize>>
        Expect<usize> read(Id id, Span<char> out, off_t offset, OnMiss on_miss)
        {
            auto start = static_cast<usize>(offset) / g_page_size;
            auto last  = (static_cast<usize>(offset) + out.size() - 1) / g_page_size;

            auto total_read = 0uz;

            for (auto index : sv::iota(start, last + 1)) {
                auto read_lock = std::shared_lock{ m_mutex };

                auto key   = PageKey{ id, index };
                auto entry = m_pages.find(key);

                if (entry == m_pages.end()) {
                    auto data = Array<std::byte, g_page_size>{};
                    auto res  = on_miss(data, static_cast<off_t>(index * g_page_size));
                    if (not res.has_value()) {
                        return Unexpect{ res.error() };
                    }

                    read_lock.unlock();
                    auto write_lock = std::unique_lock{ m_mutex };

                    m_lru.emplace_front(key);

                    auto new_entry = MapEntry{
                        .page   = Page{ { data.begin(), res.value() }, 0, false },
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
                            if (page.mapped().page.is_dirty()) {
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
                    local_offset = static_cast<usize>(offset) % g_page_size;
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
            auto start = static_cast<usize>(offset) / g_page_size;
            auto last  = (static_cast<usize>(offset) + in.size() - 1) / g_page_size;

            auto total_written = 0uz;

            for (auto index : sv::iota(start, last + 1)) {
                auto write_lock = std::unique_lock{ m_mutex };

                auto key  = PageKey{ id, index };
                auto page = m_pages.find(key);

                auto local_offset = 0uz;
                if (index == start) {
                    local_offset = static_cast<usize>(offset) % g_page_size;
                }

                auto write      = std::min(g_page_size - local_offset, in.size() - total_written);
                auto write_span = Span{
                    reinterpret_cast<const std::byte*>(in.data()) + total_written,
                    write,
                };

                if (page == m_pages.end()) {
                    m_lru.emplace_front(key);
                    auto new_entry = MapEntry{
                        .page   = Page{ write_span, local_offset, true },
                        .lru_it = m_lru.begin(),
                    };
                    m_pages.emplace(key, std::move(new_entry));
                } else {
                    page->second.page.write(write_span, local_offset);
                    page->second.page.set_dirty(true);

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
                        if (page.mapped().page.is_dirty()) {
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
            auto num_pages = size / g_page_size + (size % g_page_size == 0 ? 0 : 1);

            for (auto index : sv::iota(0uz, num_pages)) {
                auto read_lock = std::shared_lock{ m_mutex };    // should I use unique_lock here?

                auto key   = PageKey{ id, index };
                auto entry = m_pages.find(key);

                if (entry == m_pages.end()) {
                    continue;
                }

                auto& page = entry->second.page;
                if (page.is_dirty()) {
                    page.set_dirty(false);

                    auto data = page.data_as<std::byte>();
                    auto res  = on_flush(data, static_cast<off_t>(index * g_page_size));
                    if (not res.has_value()) {
                        return Unexpect{ res.error() };
                    }
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

    private:
        mutable std::shared_mutex m_mutex;

        std::vector<Pair<PageKey, Page>> m_orphan_pages;    // dirty but evicted pages

        Pages m_pages;
        Lru   m_lru;    // most recently used is at the front

        usize m_max_pages = g_max_page_cache / g_page_size;
    };
};
