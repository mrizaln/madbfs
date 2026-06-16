#include "madbfs/cache/page.hpp"

#include <madbfs-common/log.hpp>

#include <algorithm>
#include <cassert>
#include <limits>

static constexpr auto nowhere = std::numeric_limits<madbfs::usize>::max();

// page.hpp impl: Page
namespace madbfs::cache
{
    Page::Page(u8* data, usize page_size)
        : m_data{ data + page_size / 8 }                // the actual data is offset by 1/8 of page_size
        , m_dirty_map{ Span{ data, page_size / 8 } }    // the first 1/8 of page_size is for dirty map
        , m_capacity{ static_cast<u32>(page_size) }
    {
        assert(page_size % 8 == 0);
    }

    usize Page::read(Span<char> out, usize offset)
    {
        auto size = std::min(m_size - offset, out.size());
        std::copy_n(m_data + offset, size, out.data());
        return size;
    }

    usize Page::write(Span<const char> in, usize offset)
    {
        if (offset >= m_capacity) [[unlikely]] {
            log_c(__func__, "[BUG] offset exceed page size [{} vs {}]", offset, m_capacity);
            return 0;
        }

        auto end = static_cast<u32>(offset + in.size());
        if (end > m_capacity) [[unlikely]] {
            log_c(__func__, "[BUG] offset + size exceed page size [{} vs {}]", end, m_capacity);
            end = std::min(end, m_capacity);
        }

        std::copy_n(in.data(), end - offset, m_data + offset);
        m_size = std::max(end, m_size);

        return end - offset;
    }

    usize Page::truncate(usize size)
    {
        auto old_size = std::exchange(m_size, std::min(static_cast<u32>(size), m_capacity));
        if (m_size > old_size) {
            sr::fill(m_data + old_size, m_data + m_size, 0);
        }
        return m_size;
    }

    Span<u8> Page::buf()
    {
        return { m_data, m_capacity };
    }

    void Page::set_dirty(usize start, usize end)
    {
        m_dirty_map.set(start, end);
        m_dirty       = true;
        m_fully_dirty = (start == 0 and end == m_capacity);
    }

    void Page::clear_dirty()
    {
        m_dirty_map.zeroes();
        m_dirty       = false;
        m_fully_dirty = false;
    }

    void Page::set_fully_dirty()
    {
        set_dirty(0, m_capacity);
    }
}

// page.hpp impl: PageStore
namespace madbfs::cache
{
    PageStore::PageStore(usize page_size, usize max_pages)
        : m_page_size{ page_size }
        , m_max_pages{ max_pages }
    {
        assert(page_size % 8 == 0);
        assert(max_pages != 0);

        m_data.resize(max_pages * page_stride());
        m_pages.reserve(max_pages);

        for (auto i : sv::iota(0uz, max_pages)) {
            auto offset = i * page_stride();
            auto page   = Page{ m_data.data() + offset, page_size };
            m_pages.emplace_back(page, std::nullopt, i + 1, i - 1);
        }

        m_pages.front().prev = nowhere;
        m_pages.back().next  = nowhere;

        m_front = 0;
        m_back  = max_pages - 1;
    }

    KeyedPageId PageStore::acquire(Opt<PageKey> key)
    {
        to_front(m_back);

        auto& node    = m_pages[m_front];
        auto  old_key = std::exchange(node.key, key);

        return { .page_id = PageId{ m_front }, .key = old_key };
    }

    KeyedPage PageStore::get(PageId id, bool update_position)
    {
        assert(id.inner < m_max_pages);

        auto& node = m_pages[id.inner];
        assert(node.key.has_value());

        if (update_position) {
            to_front(id.inner);
        }

        return { .page = node.page, .key = *node.key };
    }

    void PageStore::release(PageId id)
    {
        assert(id.inner < m_max_pages);
        m_pages[id.inner].key = std::nullopt;
    }

    void PageStore::release_all()
    {
        sr::for_each(m_pages, [](auto& k) { k = std::nullopt; }, &PageNode::key);
    }

    usize PageStore::count() const
    {
        auto res = sr::count_if(m_pages, [](auto& n) { return n.key.has_value(); });
        return static_cast<usize>(res);
    }

    void PageStore::to_front(usize index)
    {
        auto& page = m_pages[index];

        if (page.prev == nowhere) {    // already at front
            return;
        } else if (page.next == nowhere) {    // at the back
            auto& back  = m_pages[page.prev];
            auto& after = m_pages[m_front];

            back.next  = nowhere;
            after.prev = index;

            m_back = page.prev;

            page.next = m_front;
            page.prev = nowhere;

            m_front = index;
        } else {    // anywhere else
            auto& prev  = m_pages[page.prev];
            auto& next  = m_pages[page.next];
            auto& front = m_pages[m_front];

            prev.next = page.next;
            next.prev = page.prev;

            front.prev = index;
            page.next  = m_front;
            page.prev  = nowhere;

            m_front = index;
        }
    }
}
