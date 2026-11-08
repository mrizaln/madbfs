#include "madbfs/orphan_store.hpp"

#include <cassert>
#include <fmt/format.h>

namespace madbfs
{
    Str OrphanStore::add(Id parent, Str name)
    {
        auto orphan_name = generate_orphan_name();

        auto [it, ok] = m_map.try_emplace(Key{ .parent = parent, .name = String{ name } }, orphan_name);
        assert(ok);

        return it->second;
    }

    Opt<String> OrphanStore::remove(Id parent, Str name)
    {
        if (auto value = m_map.extract(KeyRef{ .parent = parent, .name = name }); value.empty()) {
            return std::nullopt;
        } else {
            return std::move(value.mapped());
        }
    }

    bool OrphanStore::contains(Id parent, Str name) const
    {
        return m_map.contains(KeyRef{ .parent = parent, .name = name });
    }

    String OrphanStore::generate_orphan_name()
    {
        m_counter++;
        return fmt::format(".orphan-file{:020x}", ++m_counter);
    }
}
