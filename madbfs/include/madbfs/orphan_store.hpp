#pragma once

#include "madbfs/stat.hpp"

#include <unordered_map>

namespace madbfs
{
    class OrphanStore
    {
    public:
        OrphanStore() = default;

        Str add(Id parent, Str name);

        Opt<String> remove(Id parent, Str name);

        bool contains(Id parent, Str name) const;

    private:
        struct Key
        {
            Id     parent;
            String name;

            bool operator==(const Key&) const = default;
        };

        struct KeyRef
        {
            Id  parent;
            Str name;

            bool operator==(const KeyRef&) const = default;
        };

        struct KeyHash
        {
            using is_transparent = void;

            usize operator()(const KeyRef& key) const
            {
                auto id   = rapidhash(&key.parent, sizeof(key.parent));
                auto name = rapidhash(key.name.data(), sizeof(String::value_type) * key.name.size());
                return rapid_mix(id, name);
            }

            usize operator()(const Key& key) const
            {
                return (*this)(KeyRef{ .parent = key.parent, .name = key.name });
            }
        };

        struct KeyEqual
        {
            using is_transparent = void;

            bool operator()(const KeyRef& l, const KeyRef& r) const { return l == r; }
            bool operator()(const Key& l, const Key& r) const { return l == r; }
            bool operator()(const Key& l, const KeyRef& r) const { return KeyRef{ l.parent, l.name } == r; }
            bool operator()(const KeyRef& l, const Key& r) const { return l == KeyRef{ r.parent, r.name }; }
        };

        using Map = std::unordered_map<Key, String, KeyHash, KeyEqual>;

        String generate_orphan_name();

        Map   m_map;
        usize m_counter = 0;
    };
}
