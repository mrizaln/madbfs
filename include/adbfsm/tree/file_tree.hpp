#pragma once

#include "adbfsm/tree/node.hpp"

namespace adbfsm::tree
{
    class FileTree
    {
    public:
        FileTree()
            : m_root{ "/", nullptr, Directory{} }
        {
        }

        Node* traverse(const fs::path path);
        void  move(fs::path from, fs::path to);

        Node* pull(const fs::path path);
        i32   push(const fs::path path);

    private:
        Node m_root;
    };
}
