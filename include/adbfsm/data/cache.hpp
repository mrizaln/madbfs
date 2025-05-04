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
        Page(Span<const std::byte> data)
            : m_data{ std::make_unique<std::byte[]>(g_page_size) }
            , m_size{ 0 }
        {
            assert(data.size() <= g_page_size);
            std::copy_n(data.data(), data.size(), m_data.get());
            m_size = data.size();
        }

        usize size() const { return m_size; }

        template <typename T = std::byte>
            requires (sizeof(T) == 1)
        Span<const T> data_as() const
        {
            return Span{ reinterpret_cast<const T*>(m_data.get()), size() };
        }

        usize write(Span<const std::byte> data)
        {
            auto copied = std::min(data.size(), g_page_size);
            std::copy_n(data.data(), copied, m_data.get());
            return m_size = copied;
        }

        bool dirty;

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

            for (auto read_lock = std::shared_lock{ m_mutex }; auto index : sv::iota(start, last + 1)) {
                auto page = m_pages.find(PageKey{ id, index });

                if (page == m_pages.end()) {
                    auto data = Array<std::byte, g_page_size>{};
                    auto res  = on_miss(data, static_cast<off_t>(index * g_page_size));
                    if (not res.has_value()) {
                        return Unexpect{ res.error() };
                    }

                    read_lock.unlock();
                    auto write_lock = std::unique_lock{ m_mutex };

                    auto key = PageKey{ id, index };
                    m_lru.emplace_front(key);

                    auto new_entry = MapEntry{
                        .page   = Page{ { data.begin(), res.value() } },
                        .lru_it = m_lru.begin(),
                    };
                    auto [p, _] = m_pages.emplace(key, std::move(new_entry));
                    page        = p;

                    if (m_pages.size() > m_max_pages) {
                        auto delta = m_pages.size() - m_max_pages;
                        while (delta-- > 0) {
                            auto last = m_lru.back();
                            m_lru.pop_back();
                            m_pages.erase(last);
                        }
                    }

                    write_lock.unlock();
                    read_lock.lock();
                }

                auto data = page->second.page.data_as<char>();

                auto local_offset = 0uz;
                if (index == start) {
                    local_offset = static_cast<usize>(offset) % g_page_size;
                }

                auto read = std::min(data.size() - local_offset, out.size() - total_read);
                std::copy_n(data.data() + local_offset, read, out.data() + total_read);
                total_read += read;

                if (page->first != m_lru.front()) {
                    read_lock.unlock();
                    auto write_lock = std::unique_lock{ m_mutex };

                    m_lru.erase(page->second.lru_it);
                    m_lru.push_front(page->first);
                    page->second.lru_it = m_lru.begin();

                    write_lock.unlock();
                    read_lock.lock();
                }
            }

            return total_read;
        }

    private:
        mutable std::shared_mutex m_mutex;

        Pages m_pages;
        Lru   m_lru;    // most recently used is at the front

        usize m_max_pages = g_max_page_cache / g_page_size;
    };
};
