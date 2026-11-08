#include "madbfs/slotmap.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <random>
#include <set>

namespace ut = boost::ut;

using namespace madbfs::aliases;

struct Key
{
    u32 version;
    u32 index;

    static Key from_u64(u64 v) { return madbfs::from_u64<Key>(v); }
    u64        to_u64() { return madbfs::to_u64(*this); }

    auto operator<=>(const Key&) const = default;
};

struct Value
{
    String s;
    i32    i;

    Value(String s, i32 i)
        : s{ std::move(s) }
        , i{ i }
    {
    }

    auto operator<=>(const Value&) const = default;
};

using SlotMap = madbfs::SlotMap<Key, Value>;

thread_local static auto local_rng = std::mt19937{ std::random_device{}() };

String random_string(std::mt19937& rng, size_t len)
{
    static auto dist = std::uniform_int_distribution<i32>{ ' ', '~' };
    return sv::iota(0uz, len) | sv::transform([&](auto) { return dist(rng); }) | sr::to<String>();
}

i32 main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::throws, ut::log, ut::that, ut::fatal;

    "only trivial structs/classes should be able to be used as SlotKey"_test = [] {
        struct A
        {
            u32 index;
            u32 version;
        };

        struct B
        {
            u32 version;
            u32 index;
        };

        struct C
        {
            u32 index   : 32;
            u32 version : 32;
        };

        struct D
        {
            u32      index   : 32;
            u32      version : 16;
            uint16_t idk;
        };

        union UA
        {
            u32 index;
            u32 version;
            u64 idk;
        };

        class CA
        {
        public:
            u32 version;
            u32 index;
        };

        static_assert(madbfs::SlotKey<A>);
        static_assert(madbfs::SlotKey<B>);
        static_assert(not madbfs::SlotKey<C>);
        static_assert(not madbfs::SlotKey<D>);
        static_assert(not madbfs::SlotKey<UA>);
        static_assert(madbfs::SlotKey<CA>);
    };

    "value should be able to be emplaced and acquired with the resulting key"_test = [] {
        auto map = SlotMap{};

        auto expected = Value{ "hello", 42 };
        auto key      = map.emplace("hello", 42);

        auto actual = map.at(key);

        expect(actual != nullptr >> fatal);
        expect(*actual == expected);
    };

    "value can be erased/popped-out from the map with the same key returned from insertion"_test = [] {
        auto map = SlotMap{};

        auto expected = Value{ "hello", 42 };
        auto key      = map.emplace("hello", 42);

        auto actual = map.erase(key);

        expect(actual.has_value() >> fatal);
        expect(*actual == expected);
    };

    "key from erased element should not be valid"_test = [] {
        auto map = SlotMap{};

        auto key    = map.emplace("hello", 42);
        std::ignore = map.erase(key);

        auto contains = map.contains(key);
        expect(not contains);

        auto value = map.erase(key);
        expect(not value.has_value());
    };

    "map slot should be recycled"_test = [] {
        auto map = SlotMap{};

        auto key_1  = map.emplace("hello", 42);
        std::ignore = map.erase(key_1);

        auto key_2  = map.emplace("hello", 42);
        std::ignore = map.erase(key_2);

        expect(key_1.index == key_2.index);
        expect(key_1.version != key_2.version);
    };

    "slot size should be reported correctly"_test = [] {
        auto map = SlotMap{};

        expect(map.empty());
        expect(map.size() == 0);

        for (auto i : sv::iota(0, 100)) {
            auto string = random_string(local_rng, 8);
            std::ignore = map.emplace(string, i);
        }

        expect(map.size() == 100);
    };

    "map should be able to be iterated"_test = [] {
        constexpr auto string_len = 64;

        auto map  = SlotMap{};
        auto keys = Vec<Key>{};

        for (auto i : sv::iota(0, 132)) {
            auto string = random_string(local_rng, string_len);
            auto key    = map.insert(Value{ string, i });
            keys.push_back(key);
        }

        for (auto _ : sv::iota(0, 100)) {
            for (auto _ : sv::iota(0, 57)) {
                auto dist  = std::uniform_int_distribution<size_t>{ 0, keys.size() - 1 };
                auto index = dist(local_rng);
                auto key   = keys[index];
                keys.erase(keys.begin() + (long)index);
                map.erase(key);
            }

            for (auto i : sv::iota(0, 57)) {
                auto string = random_string(local_rng, string_len);
                auto key    = map.emplace(string, i);
                keys.push_back(key);
            }

            for (auto _ : sv::iota(0, 88)) {
                auto dist  = std::uniform_int_distribution<size_t>{ 0, keys.size() - 1 };
                auto index = dist(local_rng);
                auto key   = keys[index];
                keys.erase(keys.begin() + (long)index);
                map.erase(key);
            }

            for (auto i : sv::iota(0, 88)) {
                auto string = random_string(local_rng, string_len);
                auto key    = map.emplace(string, i);
                keys.push_back(key);
            }
        }

        for (auto _ : sv::iota(0, 63)) {
            auto dist  = std::uniform_int_distribution<size_t>{ 0, keys.size() - 1 };
            auto index = dist(local_rng);
            auto key   = keys[index];
            keys.erase(keys.begin() + (long)index);
            map.erase(key);
        }

        sr::shuffle(keys, local_rng);

        auto values_from_keys = std::set<Value>{};
        for (auto k : keys) {
            auto v = map.at(k);
            expect(v != nullptr >> fatal);
            values_from_keys.insert(*v);
        }

        auto values_from_iteration = std::set<Value>{};
        for (auto [k, v] : std::as_const(map)) {
            values_from_iteration.insert(v);
        }

        expect(map.size() == values_from_keys.size());
        expect(map.size() == values_from_iteration.size());
        expect(values_from_keys.size() == values_from_iteration.size());

        expect(sr::equal(values_from_keys, values_from_iteration));
    };

    "map can be composed with other c++20 ranges interface"_test = [] {
        auto map    = SlotMap{};
        auto keys   = Vec<Key>{};
        auto values = Vec<Value>{};

        for (auto i : sv::iota(0, 100)) {
            auto string = random_string(local_rng, 16);
            auto value  = Value{ string, i };
            auto key    = map.insert(auto{ value });
            keys.push_back(key);
            values.push_back(value);
        }

        sr::sort(keys);
        sr::sort(values);

        auto keys_map   = map | sv::keys | sr::to<std::set>();
        auto values_map = map | sv::values | sr::to<std::set>();

        expect(sr::equal(keys, keys_map));
        expect(sr::equal(values, values_map));

        auto keys_filtered     = keys | sv::filter([](Key& k) { return k.version % 2 == 0; });
        auto keys_filtered_map = map | sv::keys | sv::filter([](Key k) { return k.version % 2 == 0; });

        expect(sr::equal(keys_filtered, keys_filtered_map));

        auto values_vec          = Vec<Value>{ values.begin(), values.end() };
        auto values_filtered     = values_vec | sv::filter([](Value& v) { return v.i % 2 == 0; });
        auto values_filtered_map = map | sv::values | sv::filter([](Value& v) { return v.i % 2 == 0; });

        auto values_filtered_map_sorted = values_filtered_map | sr::to<Vec<Value>>();
        sr::sort(values_filtered_map_sorted);

        expect(sr::equal(values_filtered, values_filtered_map_sorted));
    };
}
