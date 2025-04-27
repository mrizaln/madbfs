#include "adbfsm/tree/node.hpp"
#include "adbfsm/data/connection.hpp"

#include <expected>
#include <sys/stat.h>

namespace adbfsm::tree
{
    Node* Directory::find(Str name) const
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; });
        return found != m_children.end() ? found->get() : nullptr;
    }

    bool Directory::erase(Str name)
    {
        return std::erase_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; }) > 0;
    }

    Expect<Pair<Node*, bool>> Directory::add_node(Uniq<Node> node, bool overwrite)
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == node->name(); });

        if (found == m_children.end()) {
            m_children.push_back(std::move(node));
            return Pair{ m_children.back().get(), false };
        } else if (overwrite) {
            *found = std::move(node);
            return Pair{ found->get(), true };
        }

        return std::unexpected{ std::errc::file_exists };
    }

    Node* Link::real_path() const
    {
        auto* current = m_target;
        while (current) {
            if (auto link = current->as<Link>(); link) {
                current = link->target();
            } else {
                break;
            }
        }
        return current;
    }

    Expect<Node*> Node::touch(Str name, Context context)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (auto* node = dir->find(name)) {
            node->refresh_stat();
            return node;
        }

        return context.connection.touch(context.path)
            .and_then([&] { return context.connection.stat(context.path); })
            .and_then([&](data::ParsedStat&& parsed) {
                auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), RegularFile{});
                return dir->add_node(std::move(node), false);
            })
            .transform([&](Pair<Node*, bool>&& pair) {
                auto [node, overwrite] = pair;
                assert(not overwrite);
                return node;
            });
    }

    Expect<Node*> Node::mkdir(Str name, Context context)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (dir->find(name) != nullptr) {
            return std::unexpected{ std::errc::file_exists };
        }

        return context.connection.mkdir(context.path)
            .and_then([&] { return context.connection.stat(context.path); })
            .and_then([&](data::ParsedStat&& parsed) {
                auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), Directory{});
                return dir->add_node(std::move(node), false);
            })
            .transform([](Pair<Node*, bool>&& pair) {
                auto [node, overwrite] = pair;
                assert(not overwrite);
                return node;
            });
    }

    Expect<Node*> Node::link(Str name, Node* target, [[maybe_unused]] Context context)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (dir->find(name) != nullptr) {
            return std::unexpected{ std::errc::file_exists };
        }

        // NOTE: we can't really make a symlink on android from adb (unless rooted device iirc), so this
        // operation actuall not creating any link on the adb device, just on the in-memory filetree.

        // dummy stat for symlink based on
        // lrw-r--r-- 1 root root 21 2024-10-05 09:19:29.000000000 +0700 /sdcard -> /storage/self/primary
        auto dummy_stat = data::Stat{
            .mode  = S_IFLNK | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,    // mode: "lrw-r--r--"
            .links = 1,
            .uid   = 0,
            .gid   = 0,
            .size  = 21,
            .mtime = Clock::to_time_t(Clock::now()),
        };

        auto node = std::make_unique<Node>(name, this, std::move(dummy_stat), Link{ target });
        auto res  = dir->add_node(std::move(node), false);

        return res.transform([](auto& pair) {
            auto [node, overwrite] = pair;
            assert(not overwrite);
            return node;
        });
    }

    Expect<void> Node::rm(Str name, bool recursive, Context context)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        auto* node = dir->find(name);
        if (node == nullptr) {
            // TODO: should have check for the file existence on the device first, since maybe the file has
            // not been stat-ed yet
            return std::unexpected{ std::errc::no_such_file_or_directory };
        }

        if (node->is<Directory>() and not recursive) {
            return std::unexpected{ std::errc::is_a_directory };
        }

        auto success = dir->erase(name);
        assert(success);

        context.cache.remove(node->id());
        return context.connection.rm(context.path, recursive);
    }

    Expect<void> Node::rmdir(Str name, Context context)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        auto* node = dir->find(name);
        if (node == nullptr) {
            // TODO: should have check for the file existence on the device first, since maybe the file has
            // not been stat-ed yet
            return std::unexpected{ std::errc::no_such_file_or_directory };
        }

        if (not node->is<Directory>()) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        context.cache.remove(node->id());
        return context.connection.rmdir(context.path);
    }
}
