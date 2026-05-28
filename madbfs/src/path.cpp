#include "madbfs/path.hpp"

#include <madbfs-common/util/split.hpp>

using namespace madbfs;

// helper functions/classes
namespace
{
    /**
     * @brief Split path string into its components.
     *
     * @param comps_buf Path components storage.
     * @param path Input path string.
     *
     * @return A cleaned version of the path (extra '/' are stripped from the beginning and the end of the
     * path).
     *
     * The components and the cleaned path are in index-based slice instead of string view.
     */
    Opt<util::Slice> split_components(Vec<util::Slice>& comps_buf, Str path)
    {
        if (path.empty() or path.front() != '/') {
            return std::nullopt;
        }

        comps_buf.clear();

        while (path.size() > 1 and path.back() == '/') {
            path.remove_suffix(1);
        }

        auto offset_prefix = 0uz;
        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
            ++offset_prefix;
        }

        if (path == "/") {
            return util::Slice{};
        }

        auto index = 1uz;

        while (index < path.size()) {
            auto current = index;
            while (current < path.size() and path[current] == '/') {
                ++current;
            }

            auto next = path.find('/', current);
            if (next == Str::npos) {
                comps_buf.emplace_back(current, path.size() - current);
                break;
            }

            comps_buf.emplace_back(current, next - current);
            index = next;
        }

        return util::Slice{ offset_prefix, path.size() };
    }
}

// path.hpp impl: Path
namespace madbfs::path
{
    Str Path::parent() const
    {
        if (is_root() or m_components.size() == 1) {
            return "/";
        }

        const auto parent = m_components[m_components.size() - 2];
        const auto size   = parent.offset + parent.size;

        return { m_path.data(), size };
    }

    Path Path::parent_path() const
    {
        return is_root() ? *this : Path{ parent(), { m_components.begin(), m_components.size() - 1 } };
    }

    Opt<PathBuf> Path::extend_copy(Str name) const
    {
        if (name.contains('/')) {
            return std::nullopt;
        }

        auto path = owned();
        path.extend(name);

        return path;
    }

    PathBuf Path::owned() const
    {
        auto path       = String{ m_path };
        auto components = Vec<Slice>{ m_components.begin(), m_components.end() };
        return { std::move(path), std::move(components) };
    }
}

// path.hpp impl: PathBuf
namespace madbfs::path
{
    Str PathBuf::parent() const
    {
        if (is_root() or m_components.size() == 1) {
            return "/";
        }

        const auto parent = m_components[m_components.size() - 2];
        const auto size   = parent.offset + parent.size;

        return { m_path.data(), size };
    }

    Path PathBuf::parent_path() const
    {
        return is_root() ? *this : Path{ parent(), { m_components.begin(), m_components.size() - 1 } };
    }

    bool PathBuf::rename(Str name)
    {
        if (name.empty() or name.contains('/') or name == "." or name == "..") {
            return false;
        } else if (is_root()) {
            return false;
        }

        auto& back = m_components.back();
        m_path.replace(back.offset, back.size, name);
        back.size = name.size();

        return true;
    }

    bool PathBuf::extend(Str name)
    {
        if (name.empty() or name.contains('/') or name == "." or name == "..") {
            return false;
        }

        if (not is_root()) {
            m_path.push_back('/');
        }

        auto offset = m_path.size();
        m_path.append(name);
        m_components.emplace_back(offset, name.size());

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

    Path PathBuf::view() const
    {
        return { m_path, m_components };
    }
}

// path.hpp impl: functions
namespace madbfs::path
{
    Opt<SemiPath> create(Str path)
    {
        auto comps = Vec<Slice>{};
        auto slice = split_components(comps, path);
        if (not slice) {
            return std::nullopt;
        }

        if (comps.empty()) {
            return SemiPath{};
        }

        auto path_path = Path{ slice->to_str(path), comps };
        return SemiPath{ std::move(comps), std::move(path_path) };
    }

    Opt<PathBuf> create_buf(String&& path_str)
    {
        auto comps = Vec<Slice>{};
        auto slice = split_components(comps, path_str);
        if (not slice) {
            return std::nullopt;
        }

        if (comps.empty()) {
            path_str = "/";
        } else {
            slice->keep_slice(path_str);
        }

        return PathBuf{ std::move(path_str), std::move(comps) };
    }

    Opt<Path> create_with(Vec<Slice>& comps_buf, Str path)
    {
        auto slice = split_components(comps_buf, path);
        if (not slice) {
            return std::nullopt;
        }

        if (comps_buf.empty()) {
            return Path{};
        }

        return Path{ slice->to_str(path), comps_buf };
    }

    PathBuf resolve(Path parent, Str path)
    {
        assert(not path.empty());

        auto parents = Vec<Str>{};
        if (path.front() != '/') {
            parents = util::split(parent.str(), '/');
        }

        util::StringSplitter{ path, '/' }.while_next([&](Str str) {
            if (str == ".") {
                return;
            } else if (str == "..") {
                if (not parents.empty()) {
                    parents.pop_back();
                }
                return;
            }
            parents.push_back(str);
        });

        if (parents.empty()) {
            return PathBuf{};
        }

        auto resolved = String{};
        for (auto path : parents) {
            resolved += '/';
            resolved += path;
        }

        return create_buf(std::move(resolved)).value();
    }
}
