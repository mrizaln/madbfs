#pragma once

#include <madbfs-common/aliases.hpp>

namespace madbfs::cache
{
    /**
     * @brief Represents dirty range [start, end)
     */
    struct DirtyRange
    {
        usize start;
        usize end;
    };

    /**
     * @class DirtyIter
     *
     * @brief Input iterator for DirtyMap.
     *
     * Returns ranges of dirty region in the `DirtyMap`.
     */
    class [[nodiscard]] DirtyIter
    {
    public:
        using value_type        = DirtyRange;
        using difference_type   = isize;
        using iterator_category = std::input_iterator_tag;

        /**
         * @class Sentinel
         *
         * @brief Mark end of iteration.
         */
        struct Sentinel
        {
        };

        /**
         * @brief Default initialized iterator is equivalent to `Sentinel`.
         */
        DirtyIter() = default;

        /**
         * @brief Initialize input iterator with value bit map.
         *
         * @param bits Bit map.
         */
        DirtyIter(Span<const u8> bits);

        /**
         * @brief Fetch the range of dirty region.
         *
         * @return Dirty region range.
         *
         * Make sure that you check whether the iteartor is at end (equivalent to `Sentinel`). If it is,
         * calling this function is undefined.
         */
        [[nodiscard]] DirtyRange operator*() const;

        /**
         * @brief Fetch next range of dirty region.
         */
        DirtyIter& operator++();

        /**
         * @brief Fetch next range of dirty region.
         *
         * @return Copy of current iterator (before modification).
         */
        DirtyIter operator++(int);

        bool operator==(Sentinel) const { return m_storage == std::nullopt; }

    private:
        Span<const u8>  m_bits    = {};
        Opt<DirtyRange> m_storage = std::nullopt;
        usize          m_index   = 0;
    };

    /**
     * @class DirtyMap
     *
     * @brief Represent byte-addressable dirty pages.
     */
    class DirtyMap
    {
    public:
        DirtyMap() = default;

        /**
         * @brief Create a new dirty map using specified bits buffer as backing storage.
         *
         * @param bits Contiguous byte array.
         *
         * The buffer must live for the entire lifetime of this instance.
         */
        DirtyMap(Span<u8> bits);

        /**
         * @brief Set bits from start to end (exclusive).
         *
         * @param start Starting index.
         * @param end Ending index.
         */
        void set(usize start, usize end);

        /**
         * @brief Unset bits from start to end (exclusive).
         *
         * @param start Starting index.
         * @param end Ending index.
         */
        void unset(usize start, usize end);

        /**
         * @brief Toggle bits from start to end (exclusive).
         *
         * @param start Starting index.
         * @param end Ending index.
         */
        void toggle(usize start, usize end);

        /**
         * @brief Assign bits from start to end (exclusive) to specified value.
         *
         * @param start Starting index.
         * @param end Ending index.
         */
        void assign(usize start, usize end, bool value);

        /**
         * @brief Zeroes all bits.
         */
        void zeroes();

        /**
         * @brief Get a new iterator for dirty region.
         */
        DirtyIter begin() const { return DirtyIter{ m_bits }; }

        /**
         * @brief Get iterator end marker.
         */
        DirtyIter::Sentinel end() const { return {}; }

    private:
        Span<u8> m_bits;
    };

    static_assert(std::input_iterator<DirtyIter>);
    static_assert(std::ranges::input_range<DirtyMap>);
}
