#pragma once

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
        FileTree()
            : m_root{ "/", nullptr, Directory{} }
        {
        }

        FileTree(Node&& root)
            : m_root{ std::move(root) }
        {
        }

        /**
         * @brief Get a node by the given path.
         *
         * @param path The path to the node.
         */
        Expect<Node*> traverse(const fs::path& path);

        /**
         * @brief Create a new file at the given path.
         *
         * @param path The path to the file.
         */
        Expect<void> touch(const fs::path& path);

        /**
         * @brief Create a new directory at the given path.
         *
         * @param path The path to the directory.
         */
        Expect<void> mkdir(const fs::path& path);

        /**
         * @brief Create a new symbolic link at the given path.
         *
         * @param path The path to the symbolic link.
         * @param target The target of the symbolic link.
         */
        Expect<void> link(const fs::path& path, const fs::path& target);

        const Node& root() const { return m_root; }

    private:
        Node m_root;
    };
}
