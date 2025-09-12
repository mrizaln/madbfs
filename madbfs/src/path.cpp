#include "madbfs/path.hpp"

#include <madbfs-common/util/split.hpp>

#include <cassert>

namespace madbfs::path
{
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

namespace madbfs::path
{
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

namespace madbfs::path
{
    // returns components and the cleaned path as slice
    Opt<Pair<Vec<Slice>, Slice>> split_components(Str path)
    {
        if (path.empty() or path.front() != '/') {
            return std::nullopt;
        }

        while (path.size() > 1 and path.back() == '/') {
            path.remove_suffix(1);
        }

        auto offset_prefix = 0uz;
        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
            ++offset_prefix;
        }

        if (path == "/") {
            return Pair{ Vec<Slice>{}, Slice{} };
        }

        auto components = Vec<Slice>{};
        auto index      = 1uz;

        while (index < path.size()) {
            auto current = index;
            while (current < path.size() and path[current] == '/') {
                ++current;
            }

            auto next = path.find('/', current);
            if (next == Str::npos) {
                components.emplace_back(current, path.size() - current);
                break;
            }

            components.emplace_back(current, next - current);
            index = next;
        }

        return Pair{ std::move(components), Slice{ offset_prefix, path.size() } };
    }

    Opt<SemiPath> create(Str path)
    {
        auto split = split_components(path);
        if (not split) {
            return std::nullopt;
        }

        auto&& [comps, slice] = *split;
        if (comps.empty()) {
            return SemiPath{};
        }

        auto path_path = Path{ slice.to_str(path), comps };
        return SemiPath{ std::move(comps), std::move(path_path) };
    }

    Opt<PathBuf> create_buf(String&& path_str)
    {
        auto split = split_components(path_str);
        if (not split) {
            return std::nullopt;
        }

        auto&& [comps, slice] = *split;
        if (comps.empty()) {
            path_str = "/";
        } else {
            slice.keep_slice(path_str);
        }

        return PathBuf{ std::move(path_str), std::move(comps) };
    }

    PathBuf resolve(madbfs::path::Path parent, madbfs::Str path)
    {
        auto parents = madbfs::Vec<madbfs::Str>{};
        if (path.front() != '/') {
            parents = madbfs::util::split(parent.str(), '/');
        }

        madbfs::util::StringSplitter{ path, '/' }.while_next([&](madbfs::Str str) {
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

        auto resolved = madbfs::String{};
        for (auto path : parents) {
            resolved += '/';
            resolved += path;
        }

        return create_buf(std::move(resolved)).value();
    }
}
