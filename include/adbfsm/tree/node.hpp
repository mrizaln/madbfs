#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/cache.hpp"
#include "adbfsm/data/connection.hpp"
#include "adbfsm/data/stat.hpp"
#include "adbfsm/path/path.hpp"
#include "adbfsm/util.hpp"

#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <shared_mutex>

namespace adbfsm::tree
{
    class Node;
    class RegularFile;
    class Directory;
    class Link;
    struct Other;
    struct Error;

    using File = Var<RegularFile, Directory, Link, Other, Error>;

    // TODO: add open flags, use them to determine if file has suitable read/write permission
    class RegularFile
    {
    public:
        friend Node;

        RegularFile() = default;

        RegularFile(RegularFile&& other)
        {
            auto lock  = util::Lock{ m_operated };
            m_open_fds = std::move(other.m_open_fds);
            m_operated = false;
        }

        bool is_open(u64 fd)
        {
            auto lock = util::Lock{ m_operated };
            return sr::find(m_open_fds, fd) != m_open_fds.end();
        }

        bool open(u64 fd)
        {
            auto lock = util::Lock{ m_operated };
            if (sr::find(m_open_fds, fd) != m_open_fds.end()) {
                return false;
            }
            m_open_fds.push_back(fd);
            return true;
        }

        bool close(u64 fd)
        {
            auto lock  = util::Lock{ m_operated };
            auto found = sr::find(m_open_fds, fd);
            if (found == m_open_fds.end()) {
                return false;
            }
            m_open_fds.erase(found);
            return true;
        }

    private:
        Vec<u64>          m_open_fds;            // used to track open files
        std::atomic<bool> m_operated = false;    // used for locking.

        // NOTE: I use atomic to save space; also this is the most suitable one in this case since each locks
        // are very short lived as seen above.
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
        Expect<Ref<Node>> find(Str name) const;

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
         *
         * - If insertion happen without overwrite, right will be null.
         * - If insertion happen with overwrite and flag right will be non-null.
         * - If overwrite happen when flag not enabled, error will be returned.
         */
        Expect<Pair<Ref<Node>, Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

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
        Node& target() const { return *m_target; }

    private:
        Node* m_target;    // how do I signal node not exist anymore?
    };

    /**
     * @class Other
     * @brief For files other than regular file, directory, or link.
     *
     * Block files, character files, socket file, etc. should use this type.
     */
    struct Other
    {
    };

    /**
     * @class Error
     * @brief For error operations.
     *
     * For optimization purpose this class cache the last failed node operation.
     */
    struct Error
    {
        Errc error;
    };

    class Node
    {
    public:
        struct Context
        {
            data::IConnection& connection;
            data::Cache&       cache;
            const path::Path&  path;    // path for connection
        };

        Node(Str name, Node* parent, data::Stat stat, File value)
            : m_id{ data::Id::incr() }
            , m_parent{ parent }
            , m_name{ name }
            , m_stat{ std::move(stat) }
            , m_value{ std::move(value) }
        {
        }

        Node(Node&&)                  = delete;
        Node& operator=(Node&& other) = delete;

        Node(const Node&)            = delete;
        Node& operator=(const Node&) = delete;

        data::Id id() const { return m_id; };

        void set_name(Str name) { m_name = name; }
        void set_parent(Node* parent) { m_parent = parent; }
        void set_stat(data::Stat stat) { m_stat = stat; }

        Str         name() const { return m_name; }
        Node*       parent() const { return m_parent; }
        const File& value() const { return m_value; }

        Expect<Ref<const data::Stat>> stat() const;

        Str           printable_type() const;
        path::PathBuf build_path() const;

        void refresh_stat() { m_stat.mtime = Clock::to_time_t(Clock::now()); }

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
         * You can set this after readdir operation for example to make sure that there is no need to do any
         * readdir again in the future.
         */
        void set_synced();

        // operations on Directory
        // -----------------------

        /**
         * @brief Traverse into child node.
         *
         * @param name The name of the child node.
         *
         * @return The child node.
         */
        Expect<Ref<Node>> traverse(Str name) const;

        /**
         * @brief List children of this node.
         *
         * @param Function to operate on these children.
         *
         * This function will acquire a shared lock, so beware if you call any function that use this node
         * that locks a unique lock (non-const functions). It will deadlock.
         */
        Expect<void> list(std::move_only_function<void(Str)>&& fn) const;

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
        Expect<Ref<Node>> build(Str name, data::Stat stat, File file);

        /**
         * @brief Extract a child node.
         *
         * @param name The name of the node.
         *
         * @return The extracted node.
         *
         * Since it extracts a child node, it only works on Directory.
         */
        Expect<Uniq<Node>> extract(Str name);

        /**
         * @brief Insert a node into another node.
         *
         * @param node The node to add.
         * @param overwrite If true, overwrite the existing node with the same name.
         *
         * @return A pair containing the added node and overwriten node if overwrite happens.
         *
         * Since the insertion is basically adding a child node, it only works on Directory.
         *
         * - If insertion happen without overwrite, left will be non-null whereas right will be null.
         * - If insertion happen with overwrite and flag enabled both will be non-null.
         * - If overwrite happen when flag not enabled, error will be returned.
         */
        Expect<Pair<Ref<Node>, Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

        /**
         * @brief Create a new child node as Link.
         *
         * @param context Context needed to communicate with device and local.
         * @param name The name of the link.
         * @param target The target of the link.
         *
         * @return The new link node.
         */
        Expect<Ref<Node>> link(Str name, Node* target);

        /**
         * @brief Create a new child node as RegularFile or touch an existing one.
         *
         * @param context Context needed to communicate with device and local.
         * @param name The name of the child node.
         *
         * @return The new regular file node.
         */
        Expect<Ref<Node>> touch(Context context, Str name);

        /**
         * @brief Create a new child node as Directory.
         *
         * @param context Context needed to communicate with device and local.
         * @param name The name of the directory.
         *
         * @return The new directory node.
         */
        Expect<Ref<Node>> mkdir(Context context, Str name);

        /**
         * @brief Remove a child node by its name (RegularFile or Directory).
         *
         * @param context Context needed to communicate with device and local.
         * @param name The name of the child node.
         * @param recursive Whether to remove recursively or not.
         */
        Expect<void> rm(Context context, Str name, bool recursive);

        /**
         * @brief Remove a child node by its name (Directory).
         *
         * @param context Context needed to communicate with device and local.
         * @param name The name of the child node.
         */
        Expect<void> rmdir(Context context, Str name);

        // -----------------------

        // operations on RegularFile
        // -------------------------

        /**
         * @brief Truncate file.
         *
         * @param context Context needed to communicate with device and local.
         * @param size The final size to truncate to.
         */
        Expect<void> truncate(Context context, off_t size);

        /**
         * @brief Open file.
         *
         * @param context Context needed to communicate with device and local.
         * @param flags Open mode flags.
         *
         * @return File descriptor.
         */
        Expect<u64> open(Context context, int flags);

        /**
         * @brief Read data from file.
         *
         * @param context Context needed to communicate with device and local.
         * @param fd File descriptor.
         * @param out The output of the data.
         * @param offset Offset to the data to be read.
         *
         * @return The number of bytes read.
         */
        Expect<usize> read(Context context, u64 fd, Span<char> out, off_t offset);

        /**
         * @brief Write data to file.
         *
         * @param context Context needed to communicate with device and local.
         * @param fd File descriptor.
         * @param in Data to be written.
         * @param offset Offset of the write pointer.
         *
         * @return The number of bytes written.
         */
        Expect<usize> write(Context context, u64 fd, Str in, off_t offset);

        /**
         * @brief Flush buffer of the file.
         *
         * @param context Context needed to communicate with device and local.
         * @param fd File descriptor.
         *
         * Basically forcing writing to the file itself instead of to the buffer in memory.
         */
        Expect<void> flush(Context context, u64 fd);

        /**
         * @brief Release file.
         *
         * @param context Context needed to communicate with device and local.
         */
        Expect<void> release(Context context, u64 fd);

        /**
         * @brief Update the timestamps of a file.
         *
         * @param context Context needed to communicate with device and local.
         */
        Expect<void> utimens(Context context);

        // -------------------------

        // operations on Link
        // ------------------

        /**
         * @brief Read a link.
         */
        Expect<Ref<Node>> readlink();

        // ------------------

    private:
        inline static std::atomic<u64> s_id_counter = 0;

        template <typename T>
        bool is() const;

        template <typename T>
        Expect<Ref<T>> as();

        template <typename T>
        Expect<Ref<const T>> as() const;

        Expect<Ref<RegularFile>> regular_file_prelude()
        {
            auto current = this;
            if (is<Link>()) {
                current = &readlink()->get();
            }

            if (auto err = as<Error>(); err.has_value()) {
                return Unexpect{ err->get().error };
            }

            if (current->is<Directory>()) {
                return Unexpect{ Errc::is_a_directory };
            } else if (current->is<Other>()) {
                return Unexpect{ Errc::permission_denied };
            }

            return current->as<RegularFile>();
        }

        data::Id    m_id;
        Node*       m_parent = nullptr;
        std::string m_name   = {};
        data::Stat  m_stat   = {};
        File        m_value;

        mutable std::shared_mutex m_mutex;
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
    Expect<Ref<T>> Node::as()
    {
        constexpr auto errc = [&] {
            if constexpr (std::same_as<Directory, T>) {
                return Errc::not_a_directory;
            }
            return Errc::invalid_argument;
        }();
        if (auto* val = std::get_if<T>(&m_value)) {
            return *val;
        }
        return Unexpect{ errc };
    }

    template <typename T>
    Expect<Ref<const T>> Node::as() const
    {
        constexpr auto errc = [] {
            if constexpr (std::same_as<Directory, T>) {
                return Errc::not_a_directory;
            }
            return Errc::invalid_argument;
        }();
        if (auto* val = std::get_if<T>(&m_value)) {
            return *val;
        }
        return Unexpect{ errc };
    }

    inline Expect<Ref<const data::Stat>> Node::stat() const
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return m_stat;
    }

    inline Str Node::printable_type() const
    {
        auto visitor = util::Overload{
            [](const RegularFile&) { return "file"; },       //
            [](const Directory&) { return "directory"; },    //
            [](const Link&) { return "link"; },              //
            [](const Other&) { return "other"; },            //
            [](const Error&) { return "error"; },            //
        };
        return std::visit(visitor, m_value);
    }

    inline path::PathBuf Node::build_path() const
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

        return path::create_buf(std::move(path)).value();
    }
}
