#include "adbfsm/tree/file_tree.hpp"

namespace adbfsm::tree
{
    Expect<Node*> FileTree::traverse(const fs::path& path)
    {
        assert(not path.empty());
        assert(path.is_absolute());

        if (path == "/") {
            return &m_root;
        }

        auto* current = &m_root;

        for (auto it = ++path.begin(); it != path.end(); ++it) {
            auto* dir = current->as<Directory>();
            if (dir == nullptr) {
                return std::unexpected{ std::errc::not_a_directory };
            }
            auto* node = dir->find(it->c_str());
            if (node == nullptr) {
                return std::unexpected{ std::errc::no_such_file_or_directory };
            }
            current = node;
        }

        return current;
    }

    Expect<void> FileTree::touch(const fs::path& path)
    {
        assert(not path.empty());
        assert(path.is_absolute());

        return traverse(path.parent_path())
            .and_then([&](Node* node) { return node->touch(path.filename().c_str()); })
            .transform(sink_void);
    }

    Expect<void> FileTree::mkdir(const fs::path& path)
    {
        assert(not path.empty());
        assert(path.is_absolute());

        return traverse(path.parent_path())
            .and_then([&](Node* node) { return node->mkdir(path.filename().c_str()); })
            .transform(sink_void);
    }

    Expect<void> FileTree::link(const fs::path& path, const fs::path& target)
    {
        assert(not path.empty());
        assert(path.is_absolute());

        assert(not target.empty());
        assert(target.is_absolute());

        // for now, disallow linking to non-existent target
        auto target_node = traverse(target);
        if (not target_node) {
            return target_node.transform(sink_void);
        }

        return traverse(path.parent_path())
            .and_then([&](Node* node) { return node->link(path.filename().c_str(), *target_node); })
            .transform(sink_void);
    }
}
