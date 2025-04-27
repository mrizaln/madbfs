#include "adbfsm/data/cache.hpp"
#include "adbfsm/data/connection.hpp"
#include "adbfsm/log.hpp"

namespace adbfsm::data
{
    const Entry* Cache::get(Id id) const
    {
        auto found = sr::find(m_entries, id, &Entry::id);
        return found == m_entries.end() ? nullptr : &*found;
    }

    Expect<const Entry*> Cache::add(IConnection& connection, path::Path path)
    {
        auto stat = connection.stat(path);
        if (not stat.has_value()) {
            return std::unexpected{ stat.error() };
        }

        auto size = static_cast<usize>(stat->stat.size);
        if (size > m_max_size) {
            return std::unexpected{ std::errc::file_too_large };
        }

        auto new_size = m_current_size + size;
        while (new_size > m_max_size and not m_entries.empty()) {
            auto entry = std::move(m_entries.front());
            m_entries.pop_front();

            const auto name = entry.path.as_path().fullpath();
            log_d({ "Cache: too big ({} > {}), removing: {:?}" }, new_size, m_max_size, name);

            std::filesystem::remove(m_cache_dir / std::to_string(entry.id.inner()));
            new_size -= entry.size;
        }

        auto& entry = m_entries.emplace_back(Id::incr(), stat->stat.size, Clock::now(), path);

        auto dest = m_cache_dir / std::to_string(entry.id.inner());
        auto file = connection.pull(path, path::create(dest.c_str()).value());

        if (not file.has_value()) {
            log_e({ "Cache: failed to pull file: [{:?}]" }, entry.path.as_path().fullpath());
            return std::unexpected{ file.error() };
        }

        return &entry;
    }

    bool Cache::remove(Id id)
    {
        auto exist = std::erase_if(m_entries, [&](const Entry& entry) { return entry.id == id; }) != 0;
        if (exist) {
            std::filesystem::remove(m_cache_dir / std::to_string(id.inner()));
        }
        return exist;
    }
}
