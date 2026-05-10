#pragma once

#include "madbfs/path.hpp"
#include "madbfs/stat.hpp"

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
     * @brief Represent a symbolic link.
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
         */
        Expect<Ref<node::Regular>> regular_file_prelude();

        /**
         * @brief Get the directory variant with various checks for other variants.
         */
        Expect<Ref<node::Directory>> directory_prelude();

        /**
         * @brief Get Error ptr value if the variant is an Error.
         *
         * This function is different from `as<Error>` since it's intended for use outside of Node. It will
         * return nullptr if the Node is not an Error instance.
         */
        const node::Error* as_error() const;

        template <typename T>
        bool is() const;

        template <typename T>
        Expect<Ref<T>> as(bool check_error = false);

        template <typename T>
        Expect<Ref<const T>> as(bool check_error = false) const;

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
    template <typename T>
    bool Node::is() const
    {
        return std::holds_alternative<T>(m_value);
    }

    template <typename T>
    Expect<Ref<T>> Node::as(bool check_error)
    {
        if (check_error) {
            if (auto* err = std::get_if<node::Error>(&m_value)) {
                return Unexpect{ err->error };
            }
        }
        if (auto* val = std::get_if<T>(&m_value)) {
            return *val;
        }
        auto errc = std::same_as<node::Directory, T> ? Errc::not_a_directory : Errc::invalid_argument;
        return Unexpect{ errc };
    }

    template <typename T>
    Expect<Ref<const T>> Node::as(bool check_error) const
    {
        if (check_error) {
            if (auto* err = std::get_if<node::Error>(&m_value)) {
                return Unexpect{ err->error };
            }
        }
        if (auto* val = std::get_if<T>(&m_value)) {
            return *val;
        }
        auto errc = std::same_as<node::Directory, T> ? Errc::not_a_directory : Errc::invalid_argument;
        return Unexpect{ errc };
    }
}
