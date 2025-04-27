#include "adbfsm/tree/file_tree.hpp"
#include "adbfsm/common.hpp"

#include <expected>

namespace adbfsm::tree
{
    // TODO: pull proper stat from the device instead of using default value
    FileTree::FileTree(data::IConnection& connection, data::ICache& cache)
        : m_root{ "/", nullptr, {}, Directory{} }
        , m_cache{ cache }
        , m_connection{ connection }
    {
    }

    Expect<Node*> FileTree::traverse_parent(path::Path path)
    {
        if (path.is_root() or path.parent() == "/") {
            return &m_root;
        }

        auto* current = &m_root;

        for (auto name : path.iter_parent() | sv::drop(1)) {
            auto* dir = current->as<Directory>();
            if (dir == nullptr) {
                return std::unexpected{ std::errc::not_a_directory };
            }
            auto* node = dir->find(name);
            if (node == nullptr) {
                return std::unexpected{ std::errc::no_such_file_or_directory };
            }
            current = node;
        }

        return current;
    }

    Expect<Node*> FileTree::traverse(path::Path path)
    {
        if (path.is_root()) {
            return &m_root;
        }

        auto* current = &m_root;

        for (auto name : path.iter() | sv::drop(1)) {
            auto* dir = current->as<Directory>();
            if (dir == nullptr) {
                return std::unexpected{ std::errc::not_a_directory };
            }
            auto* node = dir->find(name);
            if (node == nullptr) {
                return std::unexpected{ std::errc::no_such_file_or_directory };
            }
            current = node;
        }

        return current;
    }

    Expect<void> FileTree::touch(path::Path path)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_parent(path)
            .and_then([&](Node* node) { return node->touch(path.filename(), context); })
            .transform(sink_void);
    }

    Expect<void> FileTree::mkdir(path::Path path)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_parent(path)
            .and_then([&](Node* node) { return node->mkdir(path.filename(), context); })
            .transform(sink_void);
    }

    Expect<void> FileTree::link(path::Path path, path::Path target)
    {
        // for now, disallow linking to non-existent target
        auto target_node = traverse(target);
        if (not target_node) {
            return target_node.transform(sink_void);
        }

        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_parent(path)
            .and_then([&](Node* node) { return node->link(path.filename(), *target_node, context); })
            .transform(sink_void);
    }

    Expect<void> FileTree::rm(path::Path path, bool recursive)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_parent(path).and_then([&](Node* parent) {
            return parent->rm(path.filename(), recursive, context);
        });
    }

    Expect<void> FileTree::rmdir(path::Path path)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_parent(path).and_then([&](Node* parent) {
            return parent->rmdir(path.filename(), context);
        });
    }
}
