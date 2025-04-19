#include "adbfsm/tree/node.hpp"

namespace adbfsm::tree
{
    Node* Directory::find(Str name) const
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name == name; });
        return found != m_children.end() ? found->get() : nullptr;
    }

    Expect<Pair<Node*, bool>> Directory::add_node(Uniq<Node> node, bool overwrite)
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name == node->name; });

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

    Expect<Node*> Node::touch(std::string_view name)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (auto* node = dir->find(name)) {
            node->stat.mtime = Clock::now();
        }

        auto node = std::make_unique<Node>(name, this, File{});
        auto res  = dir->add_node(std::move(node), false);

        return res.transform([](auto& pair) {
            auto [node, overwrite] = pair;
            assert(not overwrite);
            return node;
        });
    }

    Expect<Node*> Node::mkdir(std::string_view name)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (dir->find(name) != nullptr) {
            return std::unexpected{ std::errc::file_exists };
        }

        auto node = std::make_unique<Node>(name, this, Directory{});
        auto res  = dir->add_node(std::move(node), false);

        return res.transform([](auto& pair) {
            auto [node, overwrite] = pair;
            assert(not overwrite);
            return node;
        });
    }

    Expect<Node*> Node::link(std::string_view name, Node* target)
    {
        auto lock = util::Lock{ m_operated };

        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (dir->find(name) != nullptr) {
            return std::unexpected{ std::errc::file_exists };
        }

        auto node = std::make_unique<Node>(name, this, Link{ target });
        auto res  = dir->add_node(std::move(node), false);

        return res.transform([](auto& pair) {
            auto [node, overwrite] = pair;
            assert(not overwrite);
            return node;
        });
    }
}
