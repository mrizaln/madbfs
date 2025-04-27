#pragma once

#include "adbfsm/data/connection.hpp"
#include "adbfsm/path/path.hpp"
#include "adbfsm/tree/node.hpp"

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

        /**
         * @brief Create a new file at the given path.
         *
         * @param path The path to the file.
         */
        Expect<void> touch(path::Path path);

        /**
         * @brief Create a new directory at the given path.
         *
         * @param path The path to the directory.
         */
        Expect<void> mkdir(path::Path path);

        /**
         * @brief Create a new symbolic link at the given path.
         *
         * @param path The path to the symbolic link.
         * @param target The target of the symbolic link.
         */
        Expect<void> link(path::Path path, path::Path target);

        /**
         * @brief Remove a file by its path
         *
         * @param path The path to the file.
         * @param recursive Set to true to allow deleting file even if it's not the leaf node.
         */
        Expect<void> rm(path::Path path, bool recursive);

        /**
         * @brief Remove a directory by its path
         *
         * @param path The path to the file.
         * @param recursive Set to true to allow deleting file even if it's not the leaf node.
         */
        Expect<void> rmdir(path::Path path);

        const Node& root() const { return m_root; }

    private:
        Expect<Node*> traverse_parent(path::Path path);

        Node               m_root;
        data::ICache&      m_cache;
        data::IConnection& m_connection;
    };
}
