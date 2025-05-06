#include "adbfsm/tree/file_tree.hpp"
#include "adbfsm/log.hpp"
#include "adbfsm/tree/node.hpp"
#include "adbfsm/util/split.hpp"

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
        auto parents = adbfsm::Vec<adbfsm::Str>{};
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
    FileTree::FileTree(data::IConnection& connection, data::Cache& cache)
        : m_root{ "/", nullptr, {}, Directory{} }
        , m_connection{ connection }
        , m_cache{ cache }
    {
    }

    FileTree::~FileTree()
    {
        // NOTE: I'm still not sure how to handle this case, to push the changes to the device, this requires
        // the path to be known, but the cache is not aware of the path, just the Id of it. So pushing the
        // changes might require traversing the whole tree to find the path of said Id since the Id currently
        // is stored in each of the nodes. Tree as whole doesn't know the ids that is available let alone the
        // corresponding path.
        auto orphaned = m_cache.get_orphan_pages();

        if (not orphaned.empty()) {
            log_e({ "{}: there are {} pages worth of unwritten data, ignoring" }, __func__, orphaned.size());
        }
    }

    Expect<Ref<Node>> FileTree::traverse(path::Path path)
    {
        if (path.is_root()) {
            return m_root;
        }

        auto* current = &m_root;

        for (auto name : path.iter() | sv::drop(1)) {
            auto next = current->traverse(name);
            if (not next.has_value()) {
                return Unexpect{ next.error() };
            }
            current = &next->get();
        }

        return *current;
    }

    Expect<Ref<Node>> FileTree::traverse_or_build(path::Path path)
    {
        if (path.is_root()) {
            // workaround to initialize root stat on getattr
            if (not m_root_initialized) {
                auto parsed = m_connection.stat(path);
                if (not parsed.has_value()) {
                    return Unexpect{ parsed.error() };
                }
                m_root.set_stat(parsed->stat);
                m_root_initialized = true;
            }
            return m_root;
        }

        auto* current      = &m_root;
        auto  current_path = path::PathBuf::root();

        // iterate until parent
        for (auto name : path.parent_path().iter() | sv::drop(1)) {
            auto res = current_path.extend(name);
            assert(res and "extend failed");

            if (auto next = current->traverse(name); next.has_value()) {
                current = &next->get();
                continue;
            }

            // get stat from device
            auto parsed = m_connection.stat(current_path.as_path());
            if (not parsed.has_value()) {
                std::ignore = current->build(name, {}, Error{ parsed.error() });
                return Unexpect{ parsed.error() };
            }
            if ((parsed->stat.mode & S_IFMT) != S_IFDIR) {
                return Unexpect{ Errc::not_a_directory };
            }

            // build the node
            auto built = current->build(name, parsed->stat, Directory{});
            if (not built.has_value()) {
                return Unexpect{ built.error() };
            }

            current = &built->get();
        }

        if (auto found = current->traverse(path.filename()); found.has_value()) {
            return found;
        }

        auto res = current_path.extend(path.filename());
        assert(res and "extend failed");

        // get stat from device
        auto parsed = m_connection.stat(current_path.as_path());
        if (not parsed.has_value()) {
            std::ignore = current->build(path.filename(), {}, Error{ parsed.error() });
            return Unexpect{ parsed.error() };
        }

        // build the node depending on the node kind
        auto built = Opt<Expect<Ref<Node>>>{};

        switch (parsed->stat.mode & S_IFMT) {
        case S_IFREG: built = current->build(path.filename(), parsed->stat, RegularFile{}); break;
        case S_IFDIR: built = current->build(path.filename(), parsed->stat, Directory{}); break;
        case S_IFLNK: {
            auto target = resolve_path(path.parent_path(), unescape(parsed->link_to));

            auto target_node = traverse_or_build(path::create(target).value());
            if (not target_node.has_value()) {
                log_e({ "{}: can't found target: {:?}" }, __func__, target);
                return Unexpect{ target_node.error() };
            }

            built = current->build(path.filename(), parsed->stat, Link{ &target_node->get() });
        } break;
        default: built = current->build(path.filename(), parsed->stat, Other{}); break;
        }

        return built.value()->get();
    }

    Expect<void> FileTree::readdir(path::Path path, Filler filler)
    {
        auto base = &m_root;

        if (not path.is_root()) {
            auto maybe_base = traverse_or_build(path);
            if (not maybe_base.has_value()) {
                return Unexpect{ maybe_base.error() };
            }
            base = &maybe_base->get();
        }

        if (base->has_synced()) {
            return base->list([&](Str name) { filler(name.data()); });
        }

        // NOTE: base must be a Directory here, since traverse_or_build below code is an else branch of
        // conditional above

        return m_connection.statdir(path).transform([&](auto stats) {
            for (auto stat : stats) {
                auto file = unescape(stat.path);

                auto built = Opt<Expect<Ref<Node>>>{};

                switch (stat.stat.mode & S_IFMT) {
                case S_IFREG: built = base->build(file, stat.stat, RegularFile{}); break;
                case S_IFDIR: built = base->build(file, stat.stat, Directory{}); break;
                case S_IFLNK: {
                    auto target      = resolve_path(path, unescape(stat.link_to));
                    auto target_node = traverse_or_build(path::create(target).value());
                    if (not target_node.has_value()) {
                        log_w({ "readdir: target can't found target: {:?}" }, target);
                        continue;
                    }
                    built = base->build(file, stat.stat, Link{ &target_node->get() });
                } break;
                default: built = base->build(file, stat.stat, Other{}); break;
                }

                if (not built.has_value()) {
                    log_e(
                        { "readdir: {} [{}/{}]" },
                        std::make_error_code(built->error()).message(),
                        path.fullpath(),
                        stat.path
                    );
                }

                filler(file.c_str());
            }

            base->set_synced();
        });
    }

    Expect<Ref<const data::Stat>> FileTree::getattr(path::Path path)
    {
        return traverse_or_build(path).and_then(&Node::stat);
    }

    Expect<Ref<Node>> FileTree::readlink(path::Path path)
    {
        return traverse_or_build(path).and_then(&Node::readlink);
    }

    Expect<Ref<Node>> FileTree::mknod(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(parent).and_then(proj(&Node::touch, context, path.filename()));
    }

    Expect<Ref<Node>> FileTree::mkdir(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(parent).and_then(proj(&Node::mkdir, context, path.filename()));
    }

    Expect<void> FileTree::unlink(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(parent).and_then(proj(&Node::rm, context, path.filename(), false));
    }

    Expect<void> FileTree::rmdir(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(parent).and_then(proj(&Node::rmdir, context, path.filename()));
    }

    Expect<void> FileTree::rename(path::Path from, path::Path to)
    {
        // NOTE: I don't think root can be moved
        if (from.is_root()) {
            return Unexpect{ Errc::operation_not_supported };
        }

        auto from_node = traverse_or_build(from);
        if (not from_node.has_value()) {
            return Unexpect{ from_node.error() };
        }

        // TODO: manage cache also
        return m_connection.mv(from, to)
            .transform([&] { return std::ref(*from_node->get().parent()); })    // non-root (always exists)
            .and_then(proj(&Node::extract, from.filename()))
            .and_then([&](Uniq<Node>&& node) {
                return traverse(to.parent_path()).and_then([&](Node& to_parent) {
                    node->set_name(to.filename());
                    node->set_parent(&to_parent);
                    return to_parent.insert(std::move(node), true);
                });
            })
            .transform(sink_void);
    }

    Expect<void> FileTree::truncate(path::Path path, off_t size)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::truncate, context, size));
    }

    Expect<u64> FileTree::open(path::Path path, int flags)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::open, context, flags));
    }

    Expect<usize> FileTree::read(path::Path path, u64 fd, Span<char> out, off_t offset)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::read, context, fd, out, offset));
    }

    Expect<usize> FileTree::write(path::Path path, u64 fd, Str in, off_t offset)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::write, context, fd, in, offset));
    }

    Expect<void> FileTree::flush(path::Path path, u64 fd)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::flush, context, fd));
    }

    Expect<void> FileTree::release(path::Path path, u64 fd)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::release, context, fd));
    }

    Expect<void> FileTree::utimens(path::Path path)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        return traverse_or_build(path).and_then(proj(&Node::utimens, context));
    }

    Expect<void> FileTree::symlink(path::Path path, path::Path target)
    {
        // for now, disallow linking to non-existent target
        auto target_node = traverse(target);
        if (not target_node) {
            return target_node.transform(sink_void);
        }

        return traverse(path.parent_path())
            .and_then(proj(&Node::link, path.filename(), &target_node->get()))
            .transform(sink_void);
    }
}
