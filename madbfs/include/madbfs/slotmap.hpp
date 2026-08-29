#pragma once

#include <madbfs-common/aliases.hpp>

#include <bit>
#include <cassert>
#include <limits>

namespace madbfs
{
    template <typename T>
    concept SlotKey = requires (T t) {
        requires sizeof(T) == sizeof(u64);

        requires std::is_class_v<T>;
        requires std::is_aggregate_v<T>;
        requires std::is_trivial_v<T>;

        { auto{ t.index } } -> std::same_as<u32>;
        { auto{ t.version } } -> std::same_as<u32>;

        // prevent bitfields
        &T::index;
        &T::version;
    };

    template <typename T>
    concept SlotValue = std::is_move_constructible_v<T>;

    struct GeneralKey
    {
        u32 index;
        u32 version;
    };

    template <SlotKey Key>
    constexpr Key new_key(u32 index, u32 version)
    {
        auto key    = Key{};
        key.index   = index;
        key.version = version;
        return key;
    }

    template <SlotKey Key>
    constexpr u64 to_u64(Key key)
    {
        return (u64(key.version) << 32) | key.index;
    }

    template <SlotKey Key>
    constexpr Key from_u64(u64 value)
    {
        auto index   = static_cast<u32>(value);
        auto version = static_cast<u32>(value >> 32);
        return new_key<Key>(index, version);
    }

    template <SlotKey Key, SlotValue Value>
    class SlotMap;

    struct SlotIterEnd
    {
    };

    template <SlotKey Key, SlotValue Value, bool Const>
    class SlotIter
    {
    public:
        friend SlotMap<Key, Value>;

        using value_type        = std::conditional_t<Const, Pair<Key, const Value&>, Pair<Key, Value&>>;
        using difference_type   = isize;
        using iterator_category = std::input_iterator_tag;

        [[nodiscard]] value_type operator*() const
        {
            assert(m_index < m_slots.size());
            auto  index = m_index - 1;
            auto  ver   = m_versions[index];
            auto  key   = new_key<Key>(index, ver);
            auto& value = m_slots[index].value();    // force

            return value_type{ key, value };
        }

        SlotIter& operator++()
        {
            if (m_index >= m_slots.size()) {
                return *this;
            }

            auto skip_blocks = m_index / 64;

            for (auto bits : m_statuses | sv::drop(skip_blocks)) {
                auto pos = m_index % 64;

                if (bits == 0) {
                    m_index += 64 - pos;
                    continue;
                }

                auto off = static_cast<u32>(std::countr_zero(bits >> pos));
                if (pos + off < 64) {
                    m_index += off + 1;
                    return *this;
                }

                m_index += 64 - pos;
            }

            return *this;
        }

        SlotIter operator++(int)
        {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(SlotIterEnd) const { return m_index >= m_slots.size(); }

    private:
        using Slot = std::conditional_t<Const, const Opt<Value>, Opt<Value>>;

        SlotIter(Span<Slot> slots, Span<const u32> versions, Span<const u64> statuses)
            : m_slots{ slots }
            , m_versions{ versions }
            , m_statuses{ statuses }
        {
            ++(*this);
        }

        Span<Slot>      m_slots;
        Span<const u32> m_versions;
        Span<const u64> m_statuses;

        u32 m_index = 0;
    };

    template <SlotKey Key, SlotValue Value>
    class SlotMap
    {
    public:
        SlotMap() = default;

        template <typename... Args>
            requires std::constructible_from<Value, Args...>
        Key emplace(Args&&... args)
        {
            auto index = find_empty();
            if (not index) {
                index = grow_storage();
            }

            m_slots[*index].emplace(std::forward<Args>(args)...);
            set_status(*index, true);
            auto version = m_versions[*index];

            ++m_live_count;

            return new_key<Key>(*index, version);
        }

        Key insert(Value&& value)
        {
            auto key = emplace(std::move(value));
            return key;
        }

        Opt<Value> erase(Key key)
        {
            if (not check_valid(key)) {
                return std::nullopt;
            }

            set_status(key.index, false);
            ++m_versions[key.index];

            --m_live_count;

            return std::exchange(m_slots[key.index], std::nullopt);
        }

        template <std::predicate<Key, Value&> Fn>
        usize erase_if(Fn fn)
        {
            auto count = 0uz;
            for (auto [k, v] : *this) {
                if (fn(k, v)) {
                    erase(k);
                    ++count;
                }
            }
            return count;
        }

        Value*       at(Key key) { return check_valid(key) ? &m_slots[key.index].value() : nullptr; }
        const Value* at(Key key) const { return check_valid(key) ? &m_slots[key.index].value() : nullptr; }

        SlotIter<Key, Value, false> begin() { return { m_slots, m_versions, m_statuses }; }
        SlotIter<Key, Value, true>  begin() const { return { m_slots, m_versions, m_statuses }; }
        SlotIterEnd                 end() const { return {}; }

        SlotIter<Key, Value, true> cbegin() const { return { m_slots, m_versions, m_statuses }; }
        SlotIterEnd                cend() const { return {}; }

        bool contains(Key key) const { return check_valid(key); }

        bool  empty() const { return size() == 0; }
        usize size() const { return m_live_count; }
        usize capacity() const { return m_slots.size(); }

    private:
        bool check_valid(Key key) const
        {
            if (key.index >= m_slots.size()) {
                return false;
            }

            auto block  = key.index / 64;
            auto pos    = key.index % 64;
            auto is_set = (m_statuses[block] & (1ull << pos)) != 0;

            return key.version == m_versions[key.index] and is_set;
        }

        u32 grow_storage()
        {
            if (m_slots.empty()) {
                m_slots.resize(64 * 8);
                m_versions.resize(64 * 8, 1);
                m_statuses.resize(8);
                return 0;
            } else {
                auto size = m_slots.size();
                m_slots.resize(m_slots.size() * 2);
                m_versions.resize(m_versions.size() * 2, 1);
                m_statuses.resize(m_statuses.size() * 2);
                return static_cast<u32>(size);
            }
        }

        Opt<u32> find_empty()
        {
            for (auto&& [i, bits] : m_statuses | sv::enumerate) {
                if (bits != std::numeric_limits<u64>::max()) {
                    return i * 64 + std::countr_zero<u64>(~bits);
                }
            }
            return std::nullopt;
        }

        void set_status(u32 index, bool status)
        {
            auto block = index / 64;
            auto pos   = index % 64;

            if (status) {
                m_statuses[block] |= 1ull << pos;
            } else {
                m_statuses[block] &= ~(1ull << pos);
            }
        }

        Vec<Opt<Value>> m_slots;

        Vec<u32> m_versions;
        Vec<u64> m_statuses;

        usize m_live_count = 0;
    };
}
