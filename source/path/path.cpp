#include "adbfsm/path/path.hpp"

namespace
{
    adbfsm::Gen<adbfsm::Str> iter_path_impl(adbfsm::Str path)
    {
        using namespace adbfsm;

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

            auto it = sr::find(path | sv::drop(index), '/');
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

namespace adbfsm::path
{
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

    PathBuf combine(Path path1, Path path2)
    {
        if (path1.is_root() and path2.is_root()) {
            return PathBuf::root();
        } else if (path2.is_root()) {
            return path1.into_buf();
        } else if (path1.is_root()) {
            return path2.into_buf();
        }

        auto pathbuf = PathBuf{};

        pathbuf.m_buf  = path1.fullpath();
        pathbuf.m_buf += path2.fullpath();

        auto path = create(pathbuf.m_buf).value();    // guaranteed to be valid

        pathbuf.m_parent_size     = path.m_dirname.size();
        pathbuf.m_basename_size   = path.m_basename.size();
        pathbuf.m_basename_offset = static_cast<usize>(path.m_basename.begin() - path.m_dirname.begin());

        return pathbuf;
    }

    Opt<PathBuf> combine(Path path, Str name)
    {
        if (name.contains('/')) {
            return std::nullopt;
        }

        auto pathbuf = PathBuf{};

        pathbuf.m_buf = path.fullpath();
        if (not path.is_root()) {
            pathbuf.m_buf += '/';
        }
        pathbuf.m_buf += name;

        pathbuf.m_parent_size     = path.fullpath().size();
        pathbuf.m_basename_size   = name.size();
        pathbuf.m_basename_offset = pathbuf.m_parent_size + not path.is_root();

        return std::move(pathbuf);
    }
}
