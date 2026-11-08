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
        // std::unordered_map::extract with heterogenous key is only available in GCC 16, my CI has GCC 15
        if (auto iter = m_map.find(KeyRef{ .parent = parent, .name = name }); iter == m_map.end()) {
            return std::nullopt;
        } else {
            return std::move(m_map.extract(iter).mapped());
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
