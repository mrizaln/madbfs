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
    private:
        i32 m_fd = -1;    // negative means not opened
    };

    class Directory
    {
    public:
        Directory() = default;

        /**
         * @brief Check if a node with the given name exists.
         *
         * @param name The name of the node to check.
         */
        Node* find(Str name) const;

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
         * @return A pair containing the added node and a boolean indicating if it was overwritten.
         */
        Expect<Pair<Node*, bool>> add_node(Uniq<Node> node, bool overwrite);

        Span<const Uniq<Node>> children() const { return m_children; }

    private:
        Vec<Uniq<Node>> m_children;
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
            : m_name{ name }
            , m_parent{ parent }
            , m_stat{ std::move(stat) }
            , m_value{ std::move(value) }
        {
        }

        Node(Node&& other)
            : Node{ std::move(other), util::Lock{ other.m_operated } }
        {
        }

        Node& operator=(Node&& other) = delete;
        Node& operator=(const Node&)  = delete;
        Node(const Node&)             = delete;

        template <typename T>
        bool is() const;

        template <typename T>
        T* as();

        template <typename T>
        const T* as() const;

        template <typename... Fs>
        decltype(auto) visit(util::Overload<Fs...>&& visitor);

        template <typename... Fs>
        decltype(auto) visit(util::Overload<Fs...>&& visitor) const;

        data::Id id() const { return m_id; }
        Str      name() const { return m_name; }
        Node*    parent() const { return m_parent; }

        const data::Stat& stat() const { return m_stat; }

        Str    printable_type() const;
        String build_path() const;

        void refresh_stat()
        {
            auto lock    = util::Lock{ m_operated };
            m_stat.mtime = Clock::to_time_t(Clock::now());
        }

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

    private:
        inline static std::atomic<u64> s_id_counter = 0;

        // proxy move constructor, with lock
        Node(Node&& other, util::Lock)
            : m_id{ other.m_id }
            , m_name{ std::move(other.m_name) }
            , m_parent{ other.m_parent }
            , m_stat{ other.m_stat }
            , m_value{ std::move(other.m_value) }
        {
            other.m_parent = nullptr;
        }

        data::Id    m_id     = {};
        std::string m_name   = {};
        Node*       m_parent = nullptr;
        data::Stat  m_stat   = {};
        File        m_value;

        mutable std::atomic<bool> m_operated = false;
    };
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

    template <typename... Fs>
    decltype(auto) Node::visit(util::Overload<Fs...>&& visitor)
    {
        auto lock = util::Lock{ m_operated };
        return std::visit(std::forward<decltype(visitor)>(visitor), m_value);
    }

    template <typename... Fs>
    decltype(auto) Node::visit(util::Overload<Fs...>&& visitor) const
    {
        auto lock = util::Lock{ m_operated };
        return std::visit(std::forward<decltype(visitor)>(visitor), m_value);
    }

    inline Str Node::printable_type() const
    {
        return visit(util::Overload{
            [](const RegularFile&) { return "file"; },
            [](const Directory&) { return "directory"; },
            [](const Link&) { return "link"; },
            [](const Other&) { return "other"; },
        });
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
