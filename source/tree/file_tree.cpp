#include "adbfsm/tree/file_tree.hpp"
#include "adbfsm/log.hpp"
#include "adbfsm/tree/node.hpp"

namespace
{
    // dumb unescape function, it will skip '\' in a string
    adbfsm::String& unescape(adbfsm::String& str)
    {
        auto left  = 0uz;
        auto right = 0uz;

        while (right < str.size()) {
            right       += str[right] == '\\';
            str[left++]  = str[right++];
        }

        str.resize(left);
        return str;
    }

    // implicitly unescape the string
    adbfsm::String resolve_path(adbfsm::Str parent, adbfsm::Str path)
    {
        auto resolved    = adbfsm::String{};
        auto num_slashes = adbfsm::sr::count(parent | adbfsm::sv::drop(1), '/');

        auto pos = 0uz;
        while (path[pos] == '/') {
            ++pos;
        }
        for (; num_slashes; --num_slashes) {
            resolved.append("../");
        }

        resolved.append(path.data() + pos, path.size() - pos);

        unescape(resolved);

        return resolved;
    }

}

namespace adbfsm::tree
{
    // TODO: pull proper stat from the device instead of using default value
    FileTree::FileTree(data::IConnection& connection, data::ICache& cache)
        : m_root{ "/", nullptr, {}, Directory{} }
        , m_cache{ cache }
        , m_connection{ connection }
    {
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
            auto node = dir->find(name);
            if (not node.has_value()) {
                return std::unexpected{ node.error() };
            }
            current = node.value();
        }

        return current;
    }

    Expect<Node*> FileTree::traverse_or_build(path::Path path)
    {
        if (path.is_root()) {
            return &m_root;
        }

        auto* current      = &m_root;
        auto  current_path = String{};

        // iterate until parent
        for (auto name : path.parent_path().iter() | sv::drop(1)) {
            auto* dir = current->as<Directory>();
            if (dir == nullptr) {
                return std::unexpected{ std::errc::not_a_directory };
            }

            auto node = dir->find(name);
            if (node.has_value()) {
                current = node.value();
                continue;
            }

            current_path += '/';
            current_path += name;

            // get stat from device
            auto parsed = m_connection.stat(path::create(current_path).value());
            if (not parsed.has_value()) {
                return std::unexpected{ parsed.error() };
            }
            if ((parsed->stat.mode & S_IFDIR) != 0) {
                return std::unexpected{ std::errc::not_a_directory };
            }

            // build the node
            auto built = current->build(name, parsed->stat, Directory{});
            if (not built.has_value()) {
                return std::unexpected{ built.error() };
            }

            current = built.value();
        }

        // current must be directory here
        if (auto found = current->as<Directory>()->find(path.filename()); found != nullptr) {
            return found;
        }

        // get stat from device
        auto parsed = m_connection.stat(path::create(current_path).value());
        if (not parsed.has_value()) {
            return std::unexpected{ parsed.error() };
        }

        // build the node depending on the node kind
        auto built = Expect<Node*>{};
        switch (parsed->stat.mode & S_IFMT) {
        case S_IFREG: built = current->build(path.filename(), parsed->stat, RegularFile{}); break;
        case S_IFDIR: built = current->build(path.filename(), parsed->stat, Directory{}); break;
        case S_IFLNK: {
            auto target = resolve_path(path.parent(), parsed->link_to);
            log_d({ "FileTree::build: getting link target: {:?}" }, target);

            auto target_node = traverse_or_build(path::create(target).value());
            if (not target_node.has_value()) {
                log_e({ "FileTree::build: target can't found target: {:?}" }, target);
                return std::unexpected{ target_node.error() };
            }

            built = current->build(path.filename(), parsed->stat, Link{ *target_node });
        } break;
        default: built = current->build(path.filename(), parsed->stat, Other{}); break;
        }

        if (not built.has_value()) {
            return std::unexpected{ built.error() };
        }

        return std::unexpected{ std::errc::no_such_file_or_directory };
    }

    Expect<const data::Stat*> FileTree::getattr(path::Path path)
    {
        auto lock = std::scoped_lock{ m_mutex };

        return traverse_or_build(path)    //
            .transform([](Node* node) { return &node->stat(); });
    }

    Expect<Node*> FileTree::readlink(path::Path path)
    {
        auto lock = std::scoped_lock{ m_mutex };

        return traverse_or_build(path)    //
            .and_then([](Node* node) { return ptr_ok_or(node->as<Link>(), std::errc::invalid_argument); })
            .transform([](Link* node) { return node->target(); });
    }

    Expect<Node*> FileTree::mknod(path::Path path)
    {
        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->touch(path.filename(), { m_connection, m_cache, path });
            });
    }

    Expect<Node*> FileTree::mkdir(path::Path path)
    {
        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->mkdir(path.filename(), { m_connection, m_cache, path });
            });
    }

    Expect<void> FileTree::unlink(path::Path path)
    {
        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->rm(path.filename(), false, { m_connection, m_cache, path });
            });
    }

    Expect<void> FileTree::rmdir(path::Path path)
    {
        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->rmdir(path.filename(), { m_connection, m_cache, path });
            });
    }

    Expect<void> FileTree::rename(path::Path from, path::Path to)
    {
        auto lock = std::scoped_lock{ m_mutex };

        auto from_node = traverse_or_build(from);
        if (not from_node.has_value()) {
            return std::unexpected{ from_node.error() };
        }

        // TODO: manage cache also
        return m_connection.mv(from, to)
            .and_then([&] { return traverse(from.parent_path()); })
            .and_then([&](Node* node) { return node->extract(from.filename()); })
            .and_then([&](Uniq<Node>&& node) {
                return traverse(to.parent_path()).and_then([&](Node* to_parent) {
                    node->set_name(to.filename());
                    node->set_parent(to_parent);
                    return to_parent->insert(std::move(node), true);
                });
            })
            .transform(sink_void);
    }

    Expect<void> FileTree::symlink(path::Path path, path::Path target)
    {
        auto lock = std::scoped_lock{ m_mutex };

        // for now, disallow linking to non-existent target
        auto target_node = traverse(target);
        if (not target_node) {
            return target_node.transform(sink_void);
        }

        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse(path.parent_path())
            .and_then([&](Node* node) { return node->link(path.filename(), *target_node, context); })
            .transform(sink_void);
    }
}
