#include "adbfsm/path/path.hpp"

namespace
{
    adbfsm::Gen<adbfsm::Str> iter_path_impl(adbfsm::Str path)
    {
        assert(not path.empty() and path.front() == '/');
        co_yield "/";

        auto index = 1uz;

        while (index < path.size()) {
            while (index < path.size() and path[index] == '/') {
                ++index;
            }
            if (index >= path.size()) {
                co_return;
            }

            auto it = adbfsm::sr::find(path | adbfsm::sv::drop(index), '/');
            if (it == path.end()) {
                auto res = path.substr(index);
                co_yield res;
                co_return;
            }

            auto pos = static_cast<std::size_t>(it - path.begin());
            auto res = path.substr(index, pos - index);

            index = pos + 1;
            co_yield res;
        }
    }
}

// TODO: verify offsets if PathBuf is root

namespace adbfsm::path
{
    Opt<PathBuf> Path::extend_copy(Str name) const
    {
        if (name.contains('/')) {
            return std::nullopt;
        }

        auto pathbuf = PathBuf{};

        pathbuf.m_buf = fullpath();

        pathbuf.m_parent_size     = pathbuf.m_buf.size();
        pathbuf.m_basename_size   = name.size();
        pathbuf.m_basename_offset = pathbuf.m_buf.size();

        if (not is_root()) {
            pathbuf.m_basename_offset += 1;
            pathbuf.m_buf             += '/';
        }

        pathbuf.m_buf += name;

        return std::move(pathbuf);
    }

    Gen<Str> Path::iter() const
    {
        return iter_path_impl(fullpath());
    }

    PathBuf Path::into_buf() const
    {
        auto pathbuf = PathBuf{};

        pathbuf.m_buf             = fullpath();
        pathbuf.m_parent_size     = m_dirname.size();
        pathbuf.m_basename_size   = m_basename.size();
        pathbuf.m_basename_offset = static_cast<usize>(m_basename.begin() - m_dirname.begin());

        return pathbuf;
    }

    bool PathBuf::extend(Str name)
    {
        if (name.empty() or name.contains('/') or name == "." or name == "..") {
            return false;
        }

        m_parent_size     = m_buf.size();
        m_basename_size   = name.size();
        m_basename_offset = m_buf.size();

        if (m_buf.size() != 1 or m_buf[0] != '/') {
            m_basename_offset += 1;
            m_buf             += '/';
        }
        m_buf += name;

        return true;
    }

    Opt<PathBuf> PathBuf::extend_copy(Str name) const
    {
        auto pathbuf = *this;
        if (pathbuf.extend(name)) {
            return pathbuf;
        }
        return std::nullopt;
    }

    Path PathBuf::as_path() const
    {
        return {
            { m_buf.data(), m_parent_size },
            { m_buf.data() + m_basename_offset, m_basename_size },
        };
    }

    Opt<PathBuf> create_buf(String&& path_str)
    {
        auto path = create(path_str);
        if (not path.has_value()) {
            return std::nullopt;
        }

        auto pathbuf = PathBuf{};

        pathbuf.m_buf             = std::move(path_str);
        pathbuf.m_parent_size     = path->parent().size();
        pathbuf.m_basename_size   = path->filename().size();
        pathbuf.m_basename_offset = static_cast<usize>(path->filename().begin() - path->parent().begin());

        return std::move(pathbuf);
    }
}
