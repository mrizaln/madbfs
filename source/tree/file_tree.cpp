#include "madbfs/tree/file_tree.hpp"
#include "madbfs/log.hpp"
#include "madbfs/tree/node.hpp"

namespace madbfs::tree
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

    AExpect<Ref<Node>> FileTree::traverse_or_build(path::Path path)
    {
        if (path.is_root()) {
            // workaround to initialize root stat on getattr
            if (not m_root_initialized) {
                auto stat = co_await m_connection.stat(path);
                if (not stat.has_value()) {
                    co_return Unexpect{ stat.error() };
                }
                m_root.set_stat(*stat);
                m_root_initialized = true;
            }
            co_return m_root;
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
            auto stat = co_await m_connection.stat(current_path.as_path());
            if (not stat.has_value()) {
                std::ignore = current->build(name, {}, Error{ stat.error() });
                co_return Unexpect{ stat.error() };
            }
            if ((stat->mode & S_IFMT) != S_IFDIR) {
                co_return Unexpect{ Errc::not_a_directory };
            }

            // build the node
            auto built = current->build(name, *stat, Directory{});
            if (not built.has_value()) {
                co_return Unexpect{ built.error() };
            }

            current = &built->get();
        }

        if (auto found = current->traverse(path.filename()); found.has_value()) {
            co_return found;
        }

        auto res = current_path.extend(path.filename());
        assert(res and "extend failed");

        // get stat from device
        auto stat = co_await m_connection.stat(current_path.as_path());
        if (not stat.has_value()) {
            std::ignore = current->build(path.filename(), {}, Error{ stat.error() });
            co_return Unexpect{ stat.error() };
        }

        const auto name = path.filename();

        switch (stat->mode & S_IFMT) {
        case S_IFREG: co_return current->build(name, *stat, RegularFile{}); break;
        case S_IFDIR: co_return current->build(name, *stat, Directory{}); break;
        case S_IFLNK: {
            auto may_target = co_await m_connection.readlink(path);
            if (not may_target) {
                co_return Unexpect{ may_target.error() };
            }
            auto& target = *may_target;

            co_return (co_await traverse_or_build(target.as_path()))
                .transform_error([&](Errc err) {
                    log_e({ "{}: can't found target: {:?}" }, __func__, target.as_path().fullpath());
                    return err;
                })
                .and_then([&](Node& node) { return current->build(name, *stat, Link{ &node }); });
        } break;
        default: co_return current->build(name, *stat, Other{}); break;
        }
    }

    AExpect<void> FileTree::readdir(path::Path path, Filler filler)
    {
        auto base = &m_root;

        if (not path.is_root()) {
            auto maybe_base = co_await traverse_or_build(path);
            if (not maybe_base.has_value()) {
                co_return Unexpect{ maybe_base.error() };
            }
            base = &maybe_base->get();
        }

        if (base->has_synced()) {
            co_return base->list([&](Str name) {
                filler(name.data());    // the underlying data is null-terminated string
            });
        }

        // NOTE: base must be a Directory here, since traverse_or_build below code is an else branch of
        // conditional above

        auto may_stats = co_await m_connection.statdir(path);
        if (not may_stats) {
            co_return Unexpect{ may_stats.error() };
        }

        auto buf = String{};
        for (auto [stat, name] : may_stats.value()) {
            auto built = Opt<Expect<Ref<Node>>>{};

            switch (stat.mode & S_IFMT) {
            case S_IFREG: built = base->build(name, stat, RegularFile{}); break;
            case S_IFDIR: built = base->build(name, stat, Directory{}); break;
            case S_IFLNK: {
                auto target     = path.extend_copy(name).value();
                auto may_target = co_await m_connection.readlink(target.as_path());
                if (not may_stats) {
                    co_return Unexpect{ may_target.error() };
                }
                built = (co_await traverse_or_build(may_target->as_path())).and_then([&](Node& node) {
                    return base->build(name, stat, Link{ &node });
                });
            } break;
            default: built = base->build(name, stat, Other{}); break;
            }

            if (not built.has_value()) {
                log_e(
                    { "readdir: {} [{}/{}]" },
                    std::make_error_code(built->error()).message(),
                    path.fullpath(),
                    name
                );
            }

            // the underlying data may be not null-terminated
            buf = name;
            filler(buf.c_str());
        }

        base->set_synced();
        co_return Expect<void>{};
    }

    AExpect<Ref<const data::Stat>> FileTree::getattr(path::Path path)
    {
        co_return (co_await traverse_or_build(path)).and_then(&Node::stat);
    }

    AExpect<Ref<Node>> FileTree::readlink(path::Path path)
    {
        co_return (co_await traverse_or_build(path)).and_then(&Node::readlink);
    }

    AExpect<Ref<Node>> FileTree::mknod(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().touch(context, path.filename());
    }

    AExpect<Ref<Node>> FileTree::mkdir(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().mkdir(context, path.filename());
    }

    AExpect<void> FileTree::unlink(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().rm(context, path.filename(), false);
    }

    AExpect<void> FileTree::rmdir(path::Path path)
    {
        auto parent  = path.parent_path();
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().rmdir(context, path.filename());
    }

    AExpect<void> FileTree::rename(path::Path from, path::Path to)
    {
        // I don't think root can be moved, :P
        if (from.is_root()) {
            co_return Unexpect{ Errc::operation_not_supported };
        }

        auto from_node = co_await traverse_or_build(from);
        if (not from_node.has_value()) {
            co_return Unexpect{ from_node.error() };
        }

        co_return (co_await m_connection.mv(from, to))
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

    AExpect<void> FileTree::truncate(path::Path path, off_t size)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().truncate(context, size);
    }

    AExpect<u64> FileTree::open(path::Path path, int flags)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().open(context, flags);
    }

    AExpect<usize> FileTree::read(path::Path path, u64 fd, Span<char> out, off_t offset)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().read(context, fd, out, offset);
    }

    AExpect<usize> FileTree::write(path::Path path, u64 fd, Str in, off_t offset)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().write(context, fd, in, offset);
    }

    AExpect<void> FileTree::flush(path::Path path, u64 fd)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().flush(context, fd);
    }

    AExpect<void> FileTree::release(path::Path path, u64 fd)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().release(context, fd);
    }

    AExpect<void> FileTree::utimens(path::Path path)
    {
        auto context = Node::Context{ m_connection, m_cache, path };
        auto node    = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().utimens(context);
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
