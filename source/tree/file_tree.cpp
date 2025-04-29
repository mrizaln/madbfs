#include "adbfsm/tree/file_tree.hpp"
#include "adbfsm/log.hpp"
#include "adbfsm/tree/node.hpp"
#include "adbfsm/util.hpp"

namespace
{
    // dumb unescape function, it will skip '\' in a string
    adbfsm::String unescape(adbfsm::Str str)
    {
        auto right     = 0uz;
        auto unescaped = adbfsm::String{};

        while (right < str.size()) {
            right     += str[right] == '\\';
            unescaped += str[right++];
        }

        return unescaped;
    }

    // resolve relative path
    adbfsm::String resolve_path(adbfsm::path::Path parent, adbfsm::Str path)
    {
        auto parents = std::vector<adbfsm::Str>{};
        if (path.front() != '/') {
            parents = adbfsm::util::split(parent.fullpath(), '/');
        }

        adbfsm::util::StringSplitter{ path, '/' }.while_next([&](adbfsm::Str str) {
            if (str == ".") {
                return;
            } else if (str == "..") {
                if (not parents.empty()) {
                    parents.pop_back();
                }
                return;
            }
            parents.push_back(str);
        });

        if (parents.empty()) {
            return "/";
        }

        auto resolved = adbfsm::String{};
        for (auto path : parents) {
            resolved += '/';
            resolved += path;
        }

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
            // workaround to initialize root stat on getattr
            if (not m_root_initialized) {
                auto parsed = m_connection.stat(path);
                if (not parsed.has_value()) {
                    return std::unexpected{ parsed.error() };
                }
                m_root.set_stat(parsed->stat);
                m_root_initialized = true;
            }
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

            current_path += '/';
            current_path += name;

            auto node = dir->find(name);
            if (node.has_value()) {
                current = node.value();
                continue;
            }

            // get stat from device
            auto parsed = m_connection.stat(path::create(current_path).value());
            if (not parsed.has_value()) {
                return std::unexpected{ parsed.error() };
            }
            if ((parsed->stat.mode & S_IFMT) != S_IFDIR) {
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
        if (auto found = current->as<Directory>()->find(path.filename()); found.has_value()) {
            return found;
        }

        current_path += '/';
        current_path += path.filename();

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
            auto target = resolve_path(path.parent_path(), unescape(parsed->link_to));

            auto target_node = traverse_or_build(path::create(target).value());
            if (not target_node.has_value()) {
                log_e({ "{}: can't found target: {:?}" }, __func__, target);
                return std::unexpected{ target_node.error() };
            }

            built = current->build(path.filename(), parsed->stat, Link{ *target_node });
        } break;
        default: built = current->build(path.filename(), parsed->stat, Other{}); break;
        }

        return built;
    }

    Expect<void> FileTree::readdir(path::Path path, Filler filler)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        auto base = &m_root;

        if (not path.is_root()) {
            auto maybe_base = traverse_or_build(path);
            if (not maybe_base.has_value()) {
                return std::unexpected{ maybe_base.error() };
            }
            base = maybe_base.value();
        }

        if (auto* dir = base->as<Directory>(); dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        } else if (dir->has_readdir()) {
            for (const auto& child : dir->children()) {
                filler(child->name().data());
            }
            return {};
        }

        // NOTE: base must be a Directory here, since traverse_or_build below code is an else branch of
        // conditional above

        return m_connection.stat_dir(path).transform([&](auto stats) {
            log_d({ "readdir: {:?}" }, path.fullpath());

            for (auto stat : stats) {
                auto file = unescape(stat.path);

                auto built = Expect<Node*>{};
                switch (stat.stat.mode & S_IFMT) {
                case S_IFREG: built = base->build(file, stat.stat, RegularFile{}); break;
                case S_IFDIR: built = base->build(file, stat.stat, Directory{}); break;
                case S_IFLNK: {
                    auto target      = resolve_path(path, unescape(stat.link_to));
                    auto target_node = traverse_or_build(path::create(target).value());
                    if (not target_node.has_value()) {
                        log_e({ "readdir: target can't found target: {:?}" }, target);
                        continue;
                    }

                    built = base->build(file, stat.stat, Link{ *target_node });
                } break;
                default: built = base->build(file, stat.stat, Other{}); break;
                }

                if (not built.has_value()) {
                    log_e(
                        { "readdir: {} [{}/{}]" },
                        std::make_error_code(built.error()).message(),
                        path.fullpath(),
                        stat.path
                    );
                }

                filler(file.c_str());
            }

            base->as<Directory>()->set_readdir();
        });
    }

    Expect<const data::Stat*> FileTree::getattr(path::Path path)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        return traverse_or_build(path)    //
            .transform([](Node* node) { return &node->stat(); });
    }

    Expect<Node*> FileTree::readlink(path::Path path)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        return traverse_or_build(path)    //
            .and_then([](Node* node) { return ptr_ok_or(node->as<Link>(), std::errc::invalid_argument); })
            .transform([](Link* node) { return node->real_path(); });
    }

    Expect<Node*> FileTree::mknod(path::Path path)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->touch(path.filename(), { m_connection, m_cache, path });
            });
    }

    Expect<Node*> FileTree::mkdir(path::Path path)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->mkdir(path.filename(), { m_connection, m_cache, path });
            });
    }

    Expect<void> FileTree::unlink(path::Path path)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->rm(path.filename(), false, { m_connection, m_cache, path });
            });
    }

    Expect<void> FileTree::rmdir(path::Path path)
    {
        log_d({ "{}: {:?}" }, __func__, path.fullpath());

        auto lock = std::scoped_lock{ m_mutex };

        auto parent = path.parent_path();
        return traverse_or_build(parent)    //
            .and_then([&](Node* node) {
                return node->rmdir(path.filename(), { m_connection, m_cache, path });
            });
    }

    Expect<void> FileTree::rename(path::Path from, path::Path to)
    {
        log_d({ "{}: {:?} -> {:?}" }, __func__, from.fullpath(), to.fullpath());

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
