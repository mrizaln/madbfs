#include "madbfs/tree/tree.hpp"

#include "madbfs/path.hpp"

#include <algorithm>

using namespace madbfs;

// this doesn't point anywhere
static constexpr auto sentinel = madbfs::Id::from_u64(0);

namespace
{
    Expect<void> create_path(const tree::Tree& tree, Id id, String& buf)
    {
        auto current = id;

        while (true) {
            auto node = tree.get(current);
            if (not node) {
                return Unexpect{ Errc::no_such_file_or_directory };
            }

            auto name = node->get().name();
            buf.insert(buf.end(), name.rbegin(), name.rend());
            if (node->get().parent() == sentinel) {
                break;
            }

            buf.push_back('/');
            current = node->get().parent();
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
        return get(parent_id)    //
            .and_then([&](Node& n) { return n.as_directory(); })
            .and_then([&](node::Directory& dir) {
                auto id = m_nodes.emplace(parent_id, name, std::move(stat), std::move(value));
                return dir.insert(name, id, overwrite).transform([&](Opt<Id> /* overwritten */) {
                    return Entry{ id, *m_nodes.at(id) };
                });
            });
    }

    Expect<Ref<Node>> Tree::get(Id id)
    {
        if (auto node = m_nodes.at(id); node) {
            return std::ref(*node);
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    Expect<Ref<const Node>> Tree::get(Id id) const
    {
        if (auto node = m_nodes.at(id); node) {
            return std::ref(*node);
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    Expect<Entry> Tree::get_child(Id parent_id, Str name)
    {
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
