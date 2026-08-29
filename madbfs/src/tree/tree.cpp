#include "madbfs/tree/tree.hpp"

// this doesn't point anywhere
static constexpr auto sentinel = madbfs::Id::from_u64(0);

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
}
