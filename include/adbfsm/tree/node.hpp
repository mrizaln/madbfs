#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/cache.hpp"
#include "adbfsm/data/connection.hpp"
#include "adbfsm/data/stat.hpp"
#include "adbfsm/tree/util.hpp"

#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cassert>

namespace adbfsm::tree
{
    class Node;
    class RegularFile;
    class Directory;
    class Link;
    class Other;

    using File = Var<RegularFile, Directory, Link, Other>;

    class RegularFile
    {
    public:
        friend Node;

        data::Id id() const { return m_id; }
        int      fd() const { return m_fd; }

    private:
        void set_id(data::Id id) { m_id = id; }
        void set_fd(int fd) { m_fd = fd; }

        data::Id m_id = {};
        int      m_fd = -1;
    };

    class Directory
    {
    public:
        Directory() = default;

        bool has_readdir() const { return m_has_readdir; }
        void set_readdir() { m_has_readdir = true; }

        /**
         * @brief Check if a node with the given name exists.
         *
         * @param name The name of the node to check.
         */
        Expect<Node*> find(Str name) const;

        /**
         * @brief Erase a file by its name.
         *
         * @param name The file name.
         * @return True if file deleted, false if the file does not exist.
         */
        bool erase(Str name);

        /**
         * @brief Add a new node.
         *
         * @param node The node to add.
         * @param overwrite If true, overwrite the existing node with the same name.
         *
         * @return A pair containing the added node and overwriten node if overwrite happens.
         */
        Expect<Opt<Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

        /**
         * @brief Extract a node.
         *
         * @param name The name of the node.
         */
        Expect<Uniq<Node>> extract(Str name);

        Span<const Uniq<Node>> children() const { return m_children; }

    private:
        Vec<Uniq<Node>> m_children;
        bool            m_has_readdir = false;
    };

    // TODO: add a way to check if the link is broken
    class Link
    {
    public:
        Link(Node* target)
            : m_target{ target }
        {
        }

        /**
         * @brief Get immediate target of the link.
         */
        Node* target() const { return m_target; }

        /**
         * @brief Get the real path of the link.
         */
        Node* real_path() const;

    private:
        Node* m_target;    // how do I signal node not exist anymore?
    };

    /**
     * @class Other
     * @brief For files other than regular file, directory, or link.
     *
     * Block files, character files, socket file, etc. should use this type.
     */
    class Other
    {
    };

    class Node
    {
    public:
        struct Context
        {
            data::IConnection& connection;
            data::ICache&      cache;
            path::Path         path;    // path for connection
        };

        Node(Str name, Node* parent, data::Stat stat, File value)
            : m_parent{ parent }
            , m_name{ name }
            , m_stat{ std::move(stat) }
            , m_value{ std::move(value) }
        {
        }

        Node(Node&&)                  = delete;
        Node& operator=(Node&& other) = delete;

        Node(const Node&)            = delete;
        Node& operator=(const Node&) = delete;

        template <typename T>
        bool is() const;

        template <typename T>
        T* as();

        template <typename T>
        const T* as() const;

        void set_name(Str name) { m_name = name; }
        void set_parent(Node* parent) { m_parent = parent; }
        void set_stat(data::Stat stat) { m_stat = stat; }

        Str         name() const { return m_name; }
        Node*       parent() const { return m_parent; }
        const File& value() const { return m_value; }

        const data::Stat& stat() const { return m_stat; }

        Str    printable_type() const;
        String build_path() const;

        void refresh_stat() { m_stat.mtime = Clock::to_time_t(Clock::now()); }

        // operations on Directory
        // -----------------------

        /**
         * @brief Create a new node without any call to connection or cache with this node as its parent.
         *
         * @param name The name of the new node.
         * @param stat Stat of the new node.
         * @param file The kind of the node.
         *
         * This function is used to build a new node from existing filetree on the device. Instead of
         * operating on the file on the device itself, this function just modify the nodes. This function
         * assume that the build process won't overwrite any node.
         */
        Expect<Node*> build(Str name, data::Stat stat, File file);

        /**
         * @brief Extract a child node.
         *
         * @param name The name of the node.
         *
         * Since it extracts a child node, it only works on Directory.
         */
        Expect<Uniq<Node>> extract(Str name);

        /**
         * @brief Insert a node into another node.
         *
         * @param node The node to be inserted.
         *
         * @return Overwritten node if overwrite happen, else nullopt.
         *
         * Since the insertion is basically adding a child node, it only works on Directory.
         */
        Expect<Opt<Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

        /**
         * @brief Create a new child node as RegularFile or touch an existing one.
         *
         * @param name The name of the child node.
         * @param context Context needed to communicate with device and local.
         */
        Expect<Node*> touch(Str name, Context context);

        /**
         * @brief Create a new child node as Directory.
         *
         * @param name The name of the directory.
         * @param context Context needed to communicate with device and local.
         */
        Expect<Node*> mkdir(Str name, Context context);

        /**
         * @brief Create a new child node as Link.
         *
         * @param name The name of the link.
         * @param target The target of the link.
         * @param context Context needed to communicate with device and local.
         */
        Expect<Node*> link(Str name, Node* target, Context context);

        /**
         * @brief Remove a child node by its name (RegularFile or Directory).
         *
         * @param name The name of the child node.
         * @param recursive Whether to remove recursively or not.
         * @param context Context needed to communicate with device and local.
         */
        Expect<void> rm(Str name, bool recursive, Context context);

        /**
         * @brief Remove a child node by its name (Directory).
         *
         * @param name The name of the child node.
         * @param context Context needed to communicate with device and local.
         */
        Expect<void> rmdir(Str name, Context context);

        // -----------------------

        // operations on RegularFile
        // -------------------------

        Expect<void>  truncate(Context context, off_t size);
        Expect<i32>   open(Context context, int flags);
        Expect<usize> read(Context context, std::span<char> out, off_t offset);
        Expect<usize> write(Context context, std::string_view in, off_t offset);
        Expect<void>  flush(Context context);
        Expect<void>  release(Context context);
        Expect<void>  utimens(Context context);

        // -------------------------

    private:
        inline static std::atomic<u64> s_id_counter = 0;

        Node*       m_parent = nullptr;
        std::string m_name   = {};
        data::Stat  m_stat   = {};
        File        m_value;
    };

    // helper function
    template <typename T>
    T* into(adbfsm::tree::Node* node)
    {
        return node->as<T>();
    }
}

// -----------------------
// template implementation
// -----------------------

namespace adbfsm::tree
{
    template <typename T>
    bool Node::is() const
    {
        return std::holds_alternative<T>(m_value);
    }

    template <typename T>
    T* Node::as()
    {
        return std::get_if<T>(&m_value);
    }

    template <typename T>
    const T* Node::as() const
    {
        return std::get_if<T>(&m_value);
    }

    inline Str Node::printable_type() const
    {
        auto visitor = util::Overload{
            [](const RegularFile&) { return "file"; },
            [](const Directory&) { return "directory"; },
            [](const Link&) { return "link"; },
            [](const Other&) { return "other"; },
        };
        return std::visit(visitor, m_value);
    }

    inline String Node::build_path() const
    {
        auto path = m_name | sv::reverse | sr::to<std::string>();
        auto iter = std::back_inserter(path);

        for (auto current = m_parent; current != nullptr; current = current->m_parent) {
            *iter = '/';
            sr::copy(current->m_name | sv::reverse, iter);
        }

        // if the last path is root, we need to remove the last /
        if (path.size() > 2 and path[path.size() - 1] == '/' and path[path.size() - 2] == '/') {
            path.pop_back();
        }
        sr::reverse(path);

        return path;
    }
}
