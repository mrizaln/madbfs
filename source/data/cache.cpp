#include "adbfsm/data/cache.hpp"
#include "adbfsm/data/connection.hpp"
#include "adbfsm/log.hpp"

namespace adbfsm::data
{
    Opt<ICache::Path> Cache::get(Id id) const
    {
        auto lock = std::shared_lock{ m_mutex };

        if (exists(id)) {
            return m_cache_dir / std::to_string(id.inner());
        }
        return std::nullopt;
    }

    bool Cache::exists(Id id) const
    {
        auto lock = std::shared_lock{ m_mutex };

        return id.inner() != 0
           and (sr::find(m_entries, id, &Entry::id) != m_entries.end()
                or sr::find(m_entries_dirty, id, &Entry::id) != m_entries_dirty.end());
    }

    bool Cache::set_dirty(Id id, bool dirty)
    {
        auto lock = std::unique_lock{ m_mutex };

        if (dirty) {
            auto found = sr::find(m_entries, id, &Entry::id);
            if (found != m_entries.end()) {
                m_entries_dirty.push_back(std::move(*found));
                m_entries.erase(found);
                return true;
            }
        } else {
            auto found = sr::find(m_entries_dirty, id, &Entry::id);
            if (found != m_entries_dirty.end()) {
                m_entries.push_back(std::move(*found));
                m_entries_dirty.erase(found);
                return true;
            }
        }
        return false;
    }

    Expect<Id> Cache::add(IConnection& connection, path::Path path)
    {
        auto lock = std::unique_lock{ m_mutex };

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
            log_w({ "Cache: too big ({} > {}), removing: {:?}" }, new_size, m_max_size, name);

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

        m_current_size = new_size;

        return entry.id;
    }

    Expect<bool> Cache::remove(IConnection& connection, Id id)
    {

        auto lock = std::unique_lock{ m_mutex };

        if (id.inner() == 0) {
            return false;
        }

        auto found = sr::find(m_entries_dirty, id, &Entry::id);
        if (found != m_entries_dirty.end()) {
            auto path = m_cache_dir / std::to_string(id.inner());
            return connection.push(path::create(path.c_str()).value(), found->path.as_path()).transform([&] {
                m_current_size -= found->size;
                return std::filesystem::remove(path);
            });
        }

        auto exist = std::erase_if(m_entries, [&](const Entry& entry) { return entry.id == id; }) != 0;
        if (exist) {
            m_current_size -= found->size;
            return std::filesystem::remove(m_cache_dir / std::to_string(id.inner()));
        }
        return exist;
    }

    Expect<bool> Cache::sync(IConnection& connection)
    {
        auto lock = std::unique_lock{ m_mutex };

        if (m_entries_dirty.empty()) {
            return false;
        }

        for (const auto& entry : m_entries_dirty) {
            auto path = m_cache_dir / std::to_string(entry.id.inner());
            auto res  = connection.push(path::create(path.c_str()).value(), entry.path.as_path());
            if (not res.has_value()) {
                return std::unexpected{ res.error() };
            }
        }
        m_entries_dirty.clear();

        return true;
    }

    Expect<bool> Cache::flush(IConnection& connection, Id id)
    {
        auto lock = std::unique_lock{ m_mutex };

        auto found = sr::find(m_entries_dirty, id, &Entry::id);
        if (found == m_entries_dirty.end()) {
            return false;
        }

        auto path = m_cache_dir / std::to_string(found->id.inner());
        return connection.push(path::create(path.c_str()).value(), found->path.as_path()).transform([] {
            return true;
        });
    }
}
