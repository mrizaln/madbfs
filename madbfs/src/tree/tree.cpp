#include "madbfs/tree/tree.hpp"

#include "madbfs/path.hpp"

#include <algorithm>

using namespace madbfs;

// this doesn't point anywhere
static constexpr auto sentinel = madbfs::Id::from_u64(0);

// point to root
static constexpr auto root_ino = madbfs::Id::from_u64(1);

namespace
{
    // don't reverse, use MAX_PATH as well
    Expect<void> create_path(const tree::Tree& tree, Id id, String& buf)
    {
        auto current = id;

        while (true) {
            auto node = tree.get(current);
            if (not node) {
                return Unexpect{ Errc::no_such_file_or_directory };
            }

            auto name = node->name();
            buf.insert(buf.end(), name.rbegin(), name.rend());
            if (node->parent() == sentinel) {
                break;
            }

            buf.push_back('/');
            current = node->parent();
        }

        sr::reverse(buf);
        return {};
    }
}

// tree.hpp impl
namespace madbfs::tree
{
    Tree::Tree(Str name, Stat stat)
    {
        auto kind = node::Directory{};
        m_root    = m_nodes.emplace(sentinel, name, std::move(stat), std::move(kind));
    }

    Expect<Entry> Tree::create_node(Id parent_id, Str name, Stat stat, File value, bool overwrite)
    {
        auto id = m_nodes.emplace(parent_id, name, std::move(stat), std::move(value));

        // refetch, may invalidated by above
        auto res = get(parent_id)    //
                       .and_then([](Node& n) { return n.as_directory(); })
                       .and_then([&](node::Directory& dir) { return dir.insert(name, id, overwrite); });

        if (not res) {
            m_nodes.erase(id);
            return Unexpect{ res.error() };
        }

        if (auto orphan = *res; orphan) {
            m_nodes.erase(*orphan);
        }

        return Entry{ id, *m_nodes.at(id) };
    }

    ExpectRef<Node> Tree::get(Id id)
    {
        if (id == root_ino) {
            id = m_root;
        }
        if (auto node = m_nodes.at(id); node) {
            return *node;
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    ExpectRef<const Node> Tree::get(Id id) const
    {
        if (id == root_ino) {
            id = m_root;
        }
        if (auto node = m_nodes.at(id); node) {
            return *node;
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    Expect<Entry> Tree::get_child(Id parent_id, Str name)
    {
        if (parent_id == root_ino) {
            parent_id = m_root;
        }

        auto parent = m_nodes.at(parent_id);
        if (not parent) {
            return Unexpect{ Errc::no_such_file_or_directory };
        }

        return get(parent_id)    //
            .and_then([&](Node& n) { return n.as_directory(); })
            .and_then([&](node::Directory& dir) { return dir.find(name); })
            .and_then([&](Id id) {    //
                return get(id).transform([&](Node& n) { return Entry{ id, n }; });
            });
    }

    Opt<Node> Tree::remove(Id id)
    {
        return m_nodes.erase(id);
    }

    Expect<path::PathBuf> Tree::build_path(Id id) const
    {
        auto buf = String{};

        if (auto res = create_path(*this, id, buf); not res) {
            return Unexpect{ res.error() };
        }

        auto pathbuf = path::create_buf(std::move(buf));
        return ok_or(std::move(pathbuf), Errc::invalid_argument);
    }

    Expect<path::PathBuf> Tree::build_path(Id parent, Str name) const
    {
        auto path = build_path(parent);
        if (path) {
            if (not path->extend(name)) {
                return Unexpect{ Errc::invalid_argument };
            }
        }
        return path;
    }

    Expect<path::Path> Tree::build_path(Id id, Vec<util::Slice>& comps_buf, String& buf) const
    {
        buf.clear();
        comps_buf.clear();

        if (auto res = create_path(*this, id, buf); not res) {
            return Unexpect{ res.error() };
        }

        auto pathbuf = path::create_with(comps_buf, buf);
        return ok_or(std::move(pathbuf), Errc::invalid_argument);
    }
}
