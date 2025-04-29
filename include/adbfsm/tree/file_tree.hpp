#pragma once

#include "adbfsm/data/connection.hpp"
#include "adbfsm/path/path.hpp"
#include "adbfsm/tree/node.hpp"

#include <functional>
#include <mutex>

namespace adbfsm::tree
{
    /**
     * @class FileTree
     * @brief A class representing a file tree structure.
     *
     * This data structure is a Trie
     */
    class FileTree
    {
    public:
        using Filler = std::function<void(const char* name)>;

        FileTree(data::IConnection& connection, data::ICache& cache);

        FileTree(Node&& root)            = delete;
        FileTree& operator=(Node&& root) = delete;

        FileTree(const Node& root)            = delete;
        FileTree& operator=(const Node& root) = delete;

        /**
         * @brief Get a node by the given path.
         *
         * @param path The path to the node.
         */
        Expect<Node*> traverse(path::Path path);

        Expect<void>              readdir(path::Path path, Filler filler);
        Expect<const data::Stat*> getattr(path::Path path);

        Expect<Node*> readlink(path::Path path);
        Expect<Node*> mknod(path::Path path);
        Expect<Node*> mkdir(path::Path path);
        Expect<void>  unlink(path::Path path);
        Expect<void>  rmdir(path::Path path);
        Expect<void>  rename(path::Path from, path::Path to);
        Expect<void>  truncate(path::Path, usize size);
        Expect<i32>   open(path::Path, usize size, std::span<char> buf, off_t offset);
        Expect<usize> read(path::Path, usize size, std::string_view str, off_t offset);
        Expect<usize> write(path::Path, usize size);
        Expect<void>  flush(path::Path path);
        Expect<void>  release(path::Path path);
        Expect<void>  utimens(path::Path path);

        Expect<void> symlink(path::Path path, path::Path target);

        const Node& root() const { return m_root; }

    private:
        /**
         * @brief Traverse the node or build a new node.
         *
         * @param path Path to the node.
         */
        Expect<Node*> traverse_or_build(path::Path path);

        Node               m_root;
        data::ICache&      m_cache;
        data::IConnection& m_connection;
        bool               m_root_initialized = false;

        std::mutex m_mutex;
    };
}
