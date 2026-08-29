#pragma once

#include <madbfs-common/util/slice.hpp>

#include "madbfs/slotmap.hpp"
#include "madbfs/stat.hpp"
#include "madbfs/tree/node.hpp"

namespace madbfs::path
{
    class Path;
    class PathBuf;
}

namespace madbfs::tree
{
    /**
     * @class Entry
     *
     * @brief Represents an entry on a `Tree`.
     */
    struct Entry
    {
        Id    id;
        Node& node;
    };

    /**
     * @class Tree
     *
     * @brief In-memory file tree.
     *
     * The tree mirrors the structure of the filesystem being represented. Each `Node` of the tree stores
     * information about the file and the kind of it. Each node can be referenced via stable `Id` that are
     * obtained from `create_node()`. `Node`s are stored inside an arena that may or may not inside the tree
     * structure.
     */
    class Tree
    {
    public:
        /**
         * @brief Create a new tree with the root named as such.
         *
         * @param name The name of the root node.
         * @param stat The file attributes of the root node.
         *
         * The name of the root node must start with '/'
         */
        Tree(Str name, Stat stat);

        /**
         * @brief Create a new node attached to the tree.
         *
         * @param parent_id Unique identifier to the parent node the new node will be contained in.
         * @param name The name of the node.
         * @param stat The file attributes of the node.
         * @param value File kind of the node.
         * @param overwrite Whether to overwrite the node of the same name contained on the same parent.
         *
         * @return The entry on the tree.
         *
         * The name of the node must not contain '/' or `\0x00`
         */
        Expect<Entry> create_node(Id parent_id, Str name, Stat stat, File value, bool overwrite = false);

        /**
         * @brief Get a reference to the `Node` pointed by `id`.
         *
         * @param id Unique identifier to an instance of `Node` in the tree.
         *
         * If the associated `Node` can't be found `ENOENT` is returned.
         *
         * The returned reference is only valid until the next `create_node()` call.
         */
        Expect<Ref<Node>> get(Id id);

        /**
         * @brief Get a const reference to the `Node` pointed by `id`.
         *
         * @param id Unique identifier to an instance of `Node` in the tree.
         *
         * Same rule apply as the non-const reference overload of this function.
         */
        Expect<Ref<const Node>> get(Id id) const;

        /**
         * @brief Get the `Entry` to a node contained within `parent_id`.
         *
         * @param parent_id Unique identifier to an instance of `Node` that may contain node with `name`.
         * @param name The name of the `Node` to query inside `parent_id`'s node instance.
         *
         * @return The entry on the tree.
         *
         * If the associated `parent_id` does not refer to any `Node` instance, `ENOENT` is returned. If the
         * `parent_id` doesn't point to a `node::Directory`, `ENOTDIR` is returned instead. `ENOENT` is also
         * returned when `name` doesn't exist in `parent_id`.
         *
         * The reference to `Node` contained within `Entry` (`Entry::node`) is only valid until the next
         * `create_node()` call.
         */
        Expect<Entry> get_child(Id parent_id, Str name);

        /**
         * @brief Remove a node from the tree arena.
         *
         * @param id Unique identifier to an active `Node` inside tree.
         *
         * @return The removed node if exist or `ENOENT` if it doesn't exist.
         *
         * This function won't touch the structure of the tree itself. It is the responsibility of the caller
         * to remove references to this node inside other parent node. This function also doesn't touch the
         * content of the removed `Node`.
         */
        Opt<Node> remove(Id id);

        /**
         * @brief Build path from node pointed by `id`
         *
         * @param id Unique identifier to active `Node` in tree.
         */
        Expect<path::PathBuf> build_path(Id id) const;

        /**
         * @brief Build path from node pointed by `id`
         *
         * @param id Unique identifier to active `Node` in tree.
         */
        Expect<path::Path> build_path(Id id, Vec<util::Slice>& comps_buf, String& buf) const;

        Node&       root_node() { return *m_nodes.at(m_root); }
        const Node& root_node() const { return *m_nodes.at(m_root); }

        Id root() const { return m_root; }

    private:
        SlotMap<Id, Node> m_nodes;
        Id                m_root;
    };
}
