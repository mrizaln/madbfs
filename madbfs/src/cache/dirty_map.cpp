#include "madbfs/cache/dirty_map.hpp"

#include <cassert>
#include <climits>
#include <ranges>

static_assert(CHAR_BIT == 8);

using namespace madbfs;

// helper functions/classes
namespace
{
    /**
     * @brief Modify bits on the range [start, end).
     *
     * @param start Start position.
     * @param end End position.
     */
    template <Invocable<u64&, u64> Fn>
    void modify(Span<u8> bits, usize start, usize end, Fn&& fn)
    {
        assert(end > start);
        assert(end <= bits.size() * 8);

        constexpr auto max     = std::numeric_limits<u64>::max();
        constexpr auto size    = sizeof(u64);
        constexpr auto bitsize = size * 8;

        // inclusive
        const auto first = start / bitsize;
        const auto last  = (end - 1) / bitsize;

        const auto first_mask = max << start % bitsize;
        const auto last_mask  = max >> (bitsize - 1 - (end - 1) % bitsize);

        auto apply = [&](usize index, u64 mask) {
            auto uint = u64{};
            std::memcpy(&uint, bits.data() + index * size, size);
            fn(uint, mask);
            std::memcpy(bits.data() + index * size, &uint, size);
        };

        if (last == first) {
            apply(first, first_mask & last_mask);
        } else {
            apply(first, first_mask);
            for (auto idx : sv::iota(first + 1, last)) {
                apply(idx, max);
            }
            apply(last, last_mask);
        }
    }
}

// dirty_map.hpp impl: DirtyIter
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

// TODO: add a count for full word (64 bits). increase the count when currently modified word is changed from
// not-full to full. decrease the count when currently modified word is changed from full to not-full. based
// on this count, we can determine if the DirtyMap is fully set.

// dirty_map.hpp impl: DirtyMap
namespace madbfs::cache
{
    DirtyMap::DirtyMap(Span<u8> bits)
        : m_bits{ bits }
    {
        assert(bits.size() % 8 == 0);
    }

    void DirtyMap::set(usize start, usize end)
    {
        modify(m_bits, start, end, [&](u64& bits, u64 mask) {
            constexpr auto full = std::numeric_limits<u64>::max();

            auto prev  = bits;
            bits      |= mask;

            m_full_count += prev != full and bits == full;
        });
    }

    void DirtyMap::unset(usize start, usize end)
    {
        modify(m_bits, start, end, [&](u64& bits, u64 mask) {
            constexpr auto full = std::numeric_limits<u64>::max();

            auto prev  = bits;
            bits      &= ~mask;

            m_full_count -= (prev == full and bits != full);
        });
    }

    void DirtyMap::toggle(usize start, usize end)
    {
        modify(m_bits, start, end, [&](u64& bits, u64 mask) {
            constexpr auto full = std::numeric_limits<u64>::max();

            auto prev  = bits;
            bits      ^= mask;

            m_full_count += (prev != full and bits == full);
            m_full_count -= (prev == full and bits != full);
        });
    }

    void DirtyMap::assign(usize start, usize end, bool value)
    {
        value ? set(start, end) : unset(start, end);
    }

    void DirtyMap::zeroes()
    {
        sr::fill(m_bits, 0);
        m_full_count = 0;
    }

    bool DirtyMap::fully_set() const
    {
        return m_full_count >= m_bits.size() / 8;
    }
}
