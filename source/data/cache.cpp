#include "adbfsm/data/cache.hpp"
#include "adbfsm/log.hpp"

namespace adbfsm::data
{
    // page size is not stored to minimize the memory usage
    Page::Page(usize page_size)
        : m_data{ std::make_unique<char[]>(page_size) }
        , m_size{ 0 }
    {
    }

    Page::Page(Page&& other)
        : Page{ std::move(other), strt::exclusive_lock{ other.m_mutex } }
    {
    }

    // move ctor proxy
    Page::Page(Page&& other, strt::exclusive_lock<strt::shared_futex_micro>)
        : m_data{ std::move(other.m_data) }
        , m_size{ other.m_size }
    {
    }

    usize Page::read(Span<char> out, usize offset)
    {
        auto lock = strt::shared_lock{ m_mutex };
        auto size = std::min((m_size & ~dirty_bit) - offset, out.size());
        std::copy_n(m_data.get() + offset, size, out.data());
        return size;
    }

    usize Page::write(Span<const char> in, usize offset)
    {
        // NOTE: offset + in.size() is always assumed to be less than or equal to page size
        auto lock = strt::exclusive_lock{ m_mutex };
        auto size = offset + in.size();
        std::copy_n(in.data(), in.size(), m_data.get() + offset);
        m_size = static_cast<u32>(size) | (m_size & dirty_bit);
        return in.size();
    }

    Expect<usize> Page::write_fn(usize offset, WriteFn fn)
    {
        // NOTE: kinda risky since there is no offset and data size check with the page size
        auto lock = strt::exclusive_lock{ m_mutex };
        auto in   = fn();
        if (not in.has_value()) {
            return Unexpect{ in.error() };
        }
        auto size = offset + in->size();
        std::copy_n(in->data(), in->size(), m_data.get() + offset);
        m_size = static_cast<u32>(size) | (m_size & dirty_bit);
        return in->size();
    }

    usize Page::size() const
    {
        auto lock = strt::shared_lock{ m_mutex };
        return m_size & ~dirty_bit;
    };

    bool Page::is_dirty() const
    {
        auto lock = strt::shared_lock{ m_mutex };
        return m_size & dirty_bit;
    }

    void Page::set_dirty(bool set)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (set) {
            m_size |= dirty_bit;
        } else {
            m_size &= ~dirty_bit;
        }
    }
}

namespace adbfsm::data
{
    Cache::Cache(usize page_size, usize max_pages)
        : m_page_size{ std::bit_ceil(page_size) }
        , m_max_pages{ max_pages }
    {
    }

    Expect<usize> Cache::read(Id id, Span<char> out, off_t offset, OnMiss on_miss)
    {
        auto start = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + out.size() - 1) / m_page_size;

        auto total_read = 0uz;

        for (auto index : sv::iota(start, last + 1)) {
            auto upgrade_lock = strt::upgradeable_lock{ m_mutex };

            auto key   = PageKey{ id, index };
            auto entry = m_pages.find(key);

            if (entry == m_pages.end()) {
                auto write_lock = strt::upgrade_lock(std::move(upgrade_lock));
                m_lru.emplace_front(key);
                auto [p, _] = m_pages.emplace(key, MapEntry{ { m_page_size }, m_lru.begin() });
                entry       = p;

                // PERF: since this temporary location is unchanging, making this static might be beneficial
                thread_local static auto data = Vec<char>{};
                data.resize(m_page_size);

                auto res = entry->second.page.write_fn(0, [&] -> Expect<Span<const char>> {
                    write_lock.unlock();    // lock must still be held before page lock
                    return on_miss(data, static_cast<off_t>(index * m_page_size)).transform([&](usize len) {
                        return Span{ data.data(), len };
                    });
                });

                if (not res.has_value()) {
                    return Unexpect{ res.error() };
                }

                write_lock.lock();
                if (m_pages.size() > m_max_pages) {
                    auto delta = m_pages.size() - m_max_pages;
                    while (delta-- > 0) {
                        auto last = m_lru.back();
                        m_lru.pop_back();
                        auto page = m_pages.extract(last);
                        if (not page.empty() and page.mapped().page.is_dirty()) {
                            m_orphan_pages.emplace_back(last, std::move(page.mapped().page));
                        }
                    }
                }
            } else {
                upgrade_lock.unlock();
            }

            auto read_lock = strt::shared_lock{ m_mutex };

            auto local_offset = 0uz;
            if (index == start) {
                local_offset = static_cast<usize>(offset) % m_page_size;
            }

            auto& page     = entry->second.page;
            auto  out_sub  = Span{ out.data() + total_read, out.size() - total_read };
            total_read    += page.read(out_sub, local_offset);

            if (entry->second.lru_it != m_lru.begin()) {
                read_lock.unlock();
                auto write_lock = strt::exclusive_lock{ m_mutex };

                m_lru.erase(entry->second.lru_it);
                m_lru.push_front(entry->first);
                entry->second.lru_it = m_lru.begin();
            }
        }

        return total_read;
    }

    Expect<usize> Cache::write(Id id, Span<const char> in, off_t offset)
    {
        auto start = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + in.size() - 1) / m_page_size;

        auto total_written = 0uz;

        for (auto index : sv::iota(start, last + 1)) {
            auto write_lock = strt::exclusive_lock{ m_mutex };

            auto key  = PageKey{ id, index };
            auto page = m_pages.find(key);

            auto local_offset = 0uz;
            if (index == start) {
                local_offset = static_cast<usize>(offset) % m_page_size;
            }

            auto write      = std::min(m_page_size - local_offset, in.size() - total_written);
            auto write_span = Span{
                reinterpret_cast<const char*>(in.data()) + total_written,
                write,
            };

            if (page == m_pages.end()) {
                m_lru.emplace_front(key);
                auto [p, _] = m_pages.emplace(key, MapEntry{ { m_page_size }, m_lru.begin() });
                page        = p;
            }

            page->second.page.write(write_span, local_offset);
            page->second.page.set_dirty(true);

            if (page->second.lru_it != m_lru.begin()) {
                m_lru.erase(page->second.lru_it);
                m_lru.push_front(page->first);
                page->second.lru_it = m_lru.begin();
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

    Expect<void> Cache::flush(Id id, usize size, OnFlush on_flush)
    {
        auto num_pages = size / m_page_size + (size % m_page_size != 0);

        for (auto index : sv::iota(0uz, num_pages)) {
            auto read_lock = strt::shared_lock{ m_mutex };

            auto key   = PageKey{ id, index };
            auto entry = m_pages.find(key);

            if (entry == m_pages.end()) {
                continue;
            }

            auto& page = entry->second.page;
            if (page.is_dirty()) {

                // PERF: since this temporary location is unchanging, making this static might be beneficial
                thread_local static auto data = Vec<char>{};
                data.resize(m_page_size);

                auto read = page.read(data, 0);
                page.set_dirty(false);

                auto read_span = Span{ data.data(), read };

                auto res = on_flush(read_span, static_cast<off_t>(index * m_page_size));
                if (not res.has_value()) {
                    return Unexpect{ res.error() };
                }
            }
        }
        return {};
    }

    Vec<Pair<Cache::PageKey, Page>> Cache::get_orphan_pages()
    {
        auto write_lock = strt::exclusive_lock{ m_mutex };
        return std::move(m_orphan_pages);
    }

    bool Cache::has_orphan_pages() const
    {
        auto read_lock = strt::shared_lock{ m_mutex };
        return not m_orphan_pages.empty();
    }

    void Cache::invalidate()
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        m_pages.clear();
        m_lru.clear();
        m_orphan_pages.clear();
        log_i({ "{}: cache invalidated" }, __func__);
    }

    void Cache::set_page_size(usize new_page_size)
    {
        auto lock   = strt::exclusive_lock{ m_mutex };
        m_page_size = new_page_size;
        m_pages.clear();
        m_lru.clear();
        m_orphan_pages.clear();
        log_i({ "{}: page size changed to: {}" }, __func__, new_page_size);
    }

    void Cache::set_max_pages(usize new_max_pages)
    {
        auto lock   = strt::exclusive_lock{ m_mutex };
        m_max_pages = new_max_pages;
        m_pages.clear();
        m_lru.clear();
        m_orphan_pages.clear();
        log_i({ "{}: max pages can be stored changed to: {}" }, __func__, new_max_pages);
    }
}
