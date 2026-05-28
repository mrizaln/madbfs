#pragma once

#include "madbfs/path.hpp"
#include "madbfs/stat.hpp"

#include <madbfs-common/util/copy_const.hpp>

#include <atomic>
#include <functional>
#include <unordered_set>

namespace madbfs
{
    class Connection;
    class Node;
}

namespace madbfs::node
{
    struct Regular;
    class Directory;
    struct Link;
    struct Other;
    struct Error;

    /**
     * @class Regular
     *
     * @brief Represent a regular file.
     */
    struct Regular
    {
        bool dirty = false;
    };

    /**
     * @class Directory
     *
     * @brief Represent a directory.
     */
    class Directory
    {
    public:
        struct NodeHash
        {
            using is_transparent = void;

            usize operator()(Str str) const { return std::hash<Str>{}(str); }
            usize operator()(const Uniq<Node>& node) const;
        };

        struct NodeEq
        {
            using is_transparent = void;

            bool operator()(Str lhs, Str rhs) const { return lhs == rhs; }
            bool operator()(Str lhs, const Uniq<Node>& rhs) const;
            bool operator()(const Uniq<Node>& lhs, Str rhs) const;
            bool operator()(const Uniq<Node>& lhs, const Uniq<Node>& rhs) const;
        };

        // NOTE: use with caution, Node::m_name field must not be modified unless the node is extracted
        using List = std::unordered_set<Uniq<Node>, NodeHash, NodeEq>;

        Directory() = default;

        bool has_readdir() const { return m_has_readdir; }
        void set_readdir(bool readdir) { m_has_readdir = readdir; }

        /**
         * @brief Check if a node with the given name exists.
         *
         * @param name The name of the node to check.
         */
        Expect<Ref<Node>> find(Str name) const;

        /**
         * @brief Erase a file by its name.
         *
         * @param name The file name.
         *
         * @return The erased node, else `std::nullopt` if not exists
         *
         * This function is semantically the same with `extract()`.
         */
        Opt<Uniq<Node>> erase(Str name);

        /**
         * @brief Add a new node.
         *
         * @param node The node to add.
         * @param overwrite If true, overwrite the existing node with the same name.
         *
         * @return A pair containing the added node and overwriten node if overwrite happens.
         *
         * - If insertion happen without overwrite, right will be null.
         * - If insertion happen with overwrite flag set, right will be non-null.
         * - If overwrite happen when overwrite flag not set, error will be returned.
         */
        Expect<Pair<Ref<Node>, Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

        /**
         * @brief Extract a node.
         *
         * @param name The name of the node.
         *
         * @return The node or `Errc::no_such_file_or_directory`
         *
         * This function is semantically the same with `erase()`.
         */
        Expect<Uniq<Node>> extract(Str name);

        List&       children() { return m_children; }
        const List& children() const { return m_children; }

    private:
        List m_children;
        bool m_has_readdir = false;
    };

    /**
     * @class Link
     *
     * @brief Represent a symbolic link (lazy).
     *
     * If the link is `std::nullopt` that means readlink hasn't been performed on this link.
     */
    struct Link
    {
        Opt<String> target = std::nullopt;
    };

    /**
     * @class Other
     *
     * @brief For files other than regular file, directory, or symbolic link.
     *
     * File types that goes into this category includes but not limited to block files, character files,
     * socket file.
     */
    struct Other
    {
    };

    /**
     * @class Error
     *
     * @brief Last FUSE operation error.
     *
     * This class doesn't represent anything in the fileystem but instead represents the last failed
     * operation on this particular node. The purpose of this class is to cache the failure so madbfs doesn't
     * need to pass the operation to the device only to have it fail again.
     */
    struct Error
    {
        Errc error;
    };
}

namespace madbfs
{
    /**
     * @brief Represent a variant of file in the filesystem.
     */
    using File = Var<node::Regular, node::Directory, node::Link, node::Other, node::Error>;

    /**
     * @class Node
     *
     * @brief Represent a node in a file tree.
     */
    class Node
    {
    public:
        using Timepoint = SteadyClock::time_point;

        Node(Str name, Node* parent, Stat stat, File value)
            : m_parent{ parent }
            , m_name{ name }
            , m_id{ Id::incr() }
            , m_stat{ std::move(stat) }
            , m_value{ std::move(value) }
        {
        }

        Node(Node&&)                  = delete;
        Node& operator=(Node&& other) = delete;

        Node(const Node&)            = delete;
        Node& operator=(const Node&) = delete;

        Id id() const { return m_id; };

        void set_name(Str name) { m_name = name; }
        void set_parent(Node* parent) { m_parent = parent; }
        void set_stat(Stat stat) { m_stat = stat; }
        void set_size(off_t size) { m_stat.size = size; }

        Str         name() const { return m_name; }
        Node*       parent() const { return m_parent; }
        const File& value() const { return m_value; }
        const Stat& stat() const { return m_stat; }

        /**
         * @brief Set expiration from current time + duration.
         *
         * @param duration Duration.
         */
        void expires_after(Seconds duration);

        /**
         * @brief Check node expiry.
         */
        bool expired() const;

        /**
         * @brief Change the file variant of the node with the new one.
         *
         * @param file The new file type.
         * @return Old file variant.
         */
        File mutate(File file);

        /**
         * @brief Build path from node.
         */
        path::PathBuf build_path() const;

        /**
         * @brief Update time metadata.
         *
         * @param atime Access time (special values apply).
         * @param mtime Modification time (special values apply)
         */
        void refresh_stat(timespec atime, timespec mtime);

        /**
         * @brief Check whether node is synced with device.
         *
         * For directory this depends on whether user has set the synced flag or not. For example, directory
         * can be created but without never doing any readdir opeartion on it, thus not synced. This one is
         * user's responsibility. For other kinds this value always true since it assume that the stats
         * are synced.
         */
        bool has_synced() const;

        /**
         * @brief Set synced flag.
         *
         * @param synced The synced status.
         *
         * You can set this after readdir operation for example to make sure that there is no need to do any
         * readdir again in the future.
         */
        void set_synced(bool synced);

        /**
         * @brief Traverse into child node.
         *
         * @param name The name of the child node.
         *
         * @return The child node.
         */
        Expect<Ref<Node>> traverse(Str name) const;

        /**
         * @brief Create a new node without any call to connection or cache with this node as its parent.
         *
         * @param name The name of the new node.
         * @param stat Stat of the new node.
         * @param file The kind of the node.
         *
         * @return The new node.
         *
         * This function is used to build a new node from existing filetree on the device. Instead of
         * operating on the file on the device itself, this function just modify the nodes. This function
         * assume that the build process won't overwrite any node.
         */
        Expect<Ref<Node>> build(Str name, Stat stat, File file);

        /**
         * @brief Get the regular file variant with various checks for other variants.
         *
         * @return The reference to Regular variant or error code if others.
         *
         * This function transparently translate Error node into error code.
         */
        template <typename Self>
        Expect<Ref<util::CopyConst<node::Regular, Self>>> as_regular(this Self&& self);

        /**
         * @brief Get the directory variant with various checks for other variants.
         *
         * @return The reference to Directory variant or error code if others.
         *
         * This function transparently translate Error node into error code.
         */
        template <typename Self>
        Expect<Ref<util::CopyConst<node::Directory, Self>>> as_directory(this Self&& self);

        /**
         * @brief Get the link variant with various checks for other variants.
         *
         * @return The reference to Link variant or error code if others.
         *
         * This function transparently translate Error node into error code.
         */
        template <typename Self>
        Expect<Ref<util::CopyConst<node::Link, Self>>> as_link(this Self&& self);

        /**
         * @brief Get Error ptr value if the variant is an Error.
         *
         * return nullptr if the Node is not an Error instance.
         */
        const node::Error* as_error() const;

        /**
         * @brief Check if node has Regular variant.
         */
        bool is_regular() const { return std::holds_alternative<node::Regular>(m_value); }

        /**
         * @brief Check if node has Directory variant.
         */
        bool is_directory() const { return std::holds_alternative<node::Directory>(m_value); }

        /**
         * @brief Check if node has Link variant.
         */
        bool is_link() const { return std::holds_alternative<node::Link>(m_value); }

        /**
         * @brief Check if node has Error variant.
         */
        bool is_error() const { return std::holds_alternative<node::Error>(m_value); }

    private:
        inline static std::atomic<u64> s_id_counter = 0;

        Node*     m_parent     = nullptr;
        String    m_name       = {};
        Id        m_id         = {};
        Stat      m_stat       = {};
        Timepoint m_expiration = Timepoint::max();
        File      m_value;
    };
}

// -----------------------
// template implementation
// -----------------------

namespace madbfs
{
    template <typename Self>
    Expect<Ref<util::CopyConst<node::Regular, Self>>> Node::as_regular(this Self&& self)
    {
        // NOTE: reading/writing special files (excluding symlink) is not possible by FUSE alone. One
        // can present them by disguising it as regular files though.
        //
        // read: - https://github.com/rpodgorny/unionfs-fuse/issues/66
        //       - https://github.com/libfuse/libfuse/issues/182

        using Ret = Expect<Ref<util::CopyConst<node::Regular, Self>>>;
        using Reg = util::CopyConst<node::Regular, Self>;

        // clang-format off
        auto overload = Overload{
            [](            Reg&       reg) -> Ret { return reg;                                             },
            [](const node::Directory&    ) -> Ret { return Unexpect{ Errc::is_a_directory };                },
            [](const node::Link&         ) -> Ret { return Unexpect{ Errc::too_many_symbolic_link_levels }; },  // mimic open(2) when no O_NOFOLLOW
            [](const node::Other&        ) -> Ret { return Unexpect{ Errc::operation_not_supported };       },
            [](const node::Error&     err) -> Ret { return Unexpect{ err.error };                           },
        };
        // clang-format on

        return std::visit(overload, std::forward_like<Self>(self.m_value));
    }

    template <typename Self>
    Expect<Ref<util::CopyConst<node::Directory, Self>>> Node::as_directory(this Self&& self)
    {
        using Ret = Expect<Ref<util::CopyConst<node::Directory, Self>>>;
        using Dir = util::CopyConst<node::Directory, Self>;

        // clang-format off
        auto overload = Overload{
            [](const node::Regular&    ) -> Ret { return Unexpect{ Errc::not_a_directory }; },
            [](            Dir&     dir) -> Ret { return dir;                               },
            [](const node::Link&       ) -> Ret { return Unexpect{ Errc::not_a_directory }; },
            [](const node::Other&      ) -> Ret { return Unexpect{ Errc::not_a_directory }; },
            [](const node::Error&   err) -> Ret { return Unexpect{ err.error };             },
        };
        // clang-format on

        return std::visit(overload, std::forward_like<Self>(self.m_value));
    }

    template <typename Self>
    Expect<Ref<util::CopyConst<node::Link, Self>>> Node::as_link(this Self&& self)
    {
        using Ret  = Expect<Ref<util::CopyConst<node::Link, Self>>>;
        using Link = util::CopyConst<node::Link, Self>;

        // clang-format off
        auto overload = Overload{
            [](const node::Regular&       ) -> Ret { return Unexpect{ Errc::invalid_argument }; },
            [](const node::Directory&     ) -> Ret { return Unexpect{ Errc::invalid_argument }; },
            [](            Link&      link) -> Ret { return link;                               },
            [](const node::Other&         ) -> Ret { return Unexpect{ Errc::invalid_argument }; },
            [](const node::Error&      err) -> Ret { return Unexpect{ err.error };              },
        };
        // clang-format on

        return std::visit(overload, std::forward_like<Self>(self.m_value));
    }
}
