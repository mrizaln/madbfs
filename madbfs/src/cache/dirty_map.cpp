#include "madbfs/cache/dirty_map.hpp"

#include <cassert>
#include <ranges>

using namespace madbfs;

namespace
{
    /**
     * @brief Modify bits on the range [start, end).
     *
     * @param start Start position.
     * @param end End position.
     */
    template <typename Fn>
    void modify(Span<u8> bits, usize start, usize end, Fn&& fn)
    {
        assert(end > start);
        assert(end <= bits.size() * 8);

        // inclusive
        const auto first = start / 8;
        const auto last  = (end - 1) / 8;

        const auto first_mask = 0xFF << start % 8;
        const auto last_mask  = 0xFF >> (7 - (end - 1) % 8);

        if (last == first) {
            fn(bits[first], static_cast<u8>(first_mask & last_mask));
        } else {

            fn(bits[first], static_cast<u8>(first_mask));
            for (auto idx : sv::iota(first + 1, last)) {
                fn(bits[idx], 0xFF);
            }
            fn(bits[last], static_cast<u8>(last_mask));
        }
    }
}

namespace madbfs::cache
{
    DirtyIter::DirtyIter(Span<const u8> bits)
        : m_bits{ bits }
    {
        ++(*this);
    }

    [[nodiscard]] DirtyRange DirtyIter::operator*() const
    {
        assert(m_storage.has_value());
        return m_storage.value();
    }

    DirtyIter& DirtyIter::operator++()
    {
        const auto total = m_bits.size() * 8;

        if (m_index >= total) {
            m_storage.reset();
            return *this;
        }

        auto byte = m_index / 8;
        auto off  = m_index % 8;
        auto curr = static_cast<u8>(m_bits[byte] >> off);

        while (true) {
            // skip zero bytes
            while (m_index < total and (m_index % 8) == 0) {
                if (m_bits[m_index / 8] != 0) {
                    break;
                }
                m_index += 8;
            }

            if (m_index >= total) {
                m_storage.reset();
                return *this;
            }

            byte = m_index / 8;
            off  = m_index % 8;
            curr = static_cast<u8>(m_bits[byte] >> off);

            if (curr != 0) {
                break;
            }

            m_index = (byte + 1) * 8;    // go to next byte
        }

        const auto run   = static_cast<usize>(std::countr_zero(curr));
        const auto start = m_index + run;

        m_index += run;
        off     += run;

        while (m_index < total) {
            curr = static_cast<u8>(m_bits[byte] >> off);
            if (curr == 0) {
                break;
            }

            const auto run = static_cast<usize>(std::countr_one(curr));

            m_index += run;

            // If the run ended before the end of the available bits in this slice,
            // then we hit a zero bit and the range is done.
            if (run < 8 - off) {
                break;
            }

            byte = m_index / 8;
            off  = m_index % 8;
        }

        m_storage.emplace(start, m_index);
        return *this;
    }

    DirtyIter DirtyIter::operator++(int)
    {
        auto tmp = *this;
        ++(*this);
        return tmp;
    }
}

namespace madbfs::cache
{
    DirtyMap::DirtyMap(Span<u8> bits)
        : m_bits{ bits }
    {
    }

    void DirtyMap::set(usize start, usize end)
    {
        modify(m_bits, start, end, [](u8& byte, u8 mask) { byte |= mask; });
    }

    void DirtyMap::unset(usize start, usize end)
    {
        modify(m_bits, start, end, [](u8& byte, u8 mask) { byte &= ~mask; });
    }

    void DirtyMap::toggle(usize start, usize end)
    {
        modify(m_bits, start, end, [](u8& byte, u8 mask) { byte ^= mask; });
    }

    void DirtyMap::assign(usize start, usize end, bool value)
    {
        value ? set(start, end) : unset(start, end);
    }

    void DirtyMap::zeroes()
    {
        sr::fill(m_bits, 0);
    }
}
