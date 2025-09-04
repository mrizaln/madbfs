#include "madbfs/tree/file_tree.hpp"

#include "madbfs/tree/node.hpp"

#include <madbfs-common/log.hpp>

namespace madbfs::tree
{
    FileTree::FileTree(connection::Connection& connection, data::Cache& cache, Opt<Duration> ttl)
        : m_root{ "/", nullptr, {}, node::Directory{} }
        , m_connection{ connection }
        , m_cache{ cache }
        , m_ttl{ ttl }
    {
    }

    AExpect<Ref<Node>> FileTree::build(Node& parent, path::Path path)
    {
        const auto name = path.filename();

        auto build_then_expire = [&](Str name, data::Stat stat, File file) {
            return parent    //
                .build(name, std::move(stat), std::move(file))
                .transform([&](Node& node) {
                    node.expires_after(m_ttl.value_or(Duration::max()));
                    return std::ref(node);
                });
        };

        auto stat = co_await m_connection.stat(path);
        if (not stat.has_value()) {
            std::ignore = build_then_expire(name, {}, node::Error{ stat.error() });
            co_return Unexpect{ stat.error() };
        }

        switch (stat->mode & S_IFMT) {
        case S_IFREG: co_return build_then_expire(name, *stat, node::Regular{});
        case S_IFDIR: co_return build_then_expire(name, *stat, node::Directory{});
        case S_IFLNK: {
            if (auto target = co_await m_connection.readlink(path); target) {
                co_return build_then_expire(name, *stat, node::Link{ std::move(target).value() });
            } else {
                co_return build_then_expire(name, *stat, node::Error{ target.error() });
            }
        }
        default: co_return build_then_expire(name, *stat, node::Other{});
        }
    }

    AExpect<Ref<Node>> FileTree::build_directory(Node& parent, path::Path path)
    {
        const auto name = path.filename();

        auto build_then_expire = [&](Str name, data::Stat stat, File file) {
            return parent    //
                .build(name, std::move(stat), std::move(file))
                .transform([&](Node& node) {
                    node.expires_after(m_ttl.value_or(Duration::max()));
                    return std::ref(node);
                });
        };

        auto stat = co_await m_connection.stat(path);
        if (not stat.has_value()) {
            std::ignore = build_then_expire(name, {}, node::Error{ stat.error() });
            co_return Unexpect{ stat.error() };
        } else if ((stat->mode & S_IFMT) != S_IFDIR) {
            co_return Unexpect{ Errc::not_a_directory };
        }

        co_return build_then_expire(name, *stat, node::Directory{});
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
            current_path.extend(name);
            if (auto next = current->traverse(name); next.has_value()) {
                if (auto& node = next->get(); node.expired()) {
                    if (auto res = co_await update(node, current_path.as_path()); not res) {
                        co_return Unexpect{ res.error() };
                    }
                }
                current = &next->get();
                continue;
            }

            auto next = co_await build_directory(*current, current_path.as_path());
            if (not next) {
                co_return Unexpect{ next.error() };
            }

            current = &next->get();
        }

        current_path.extend(path.filename());
        if (auto found = current->traverse(path.filename()); found.has_value()) {
            if (auto& node = found->get(); node.expired()) {
                if (auto res = co_await update(node, current_path.as_path()); not res) {
                    co_return Unexpect{ res.error() };
                }
            }
            co_return found;
        }

        co_return co_await build(*current, current_path.as_path());
    }

    AExpect<void> FileTree::update(Node& node, path::Path path)
    {
        log_d("{}: {:?}", __func__, path.fullpath());

        auto new_stat = co_await m_connection.stat(path);
        auto old_stat = node.stat();

        // NOTE: Errc::not_connected and Errc::timed_out should be considered an OK error since it can happen
        // if the device is disconnected. The error should not be saved into the node.

        if (not new_stat) {
            auto err = new_stat.error();
            if (err != Errc::not_connected and err != Errc::timed_out) {
                node.mutate(node::Error{ err });
                node.expires_after(m_ttl.value_or(Duration::max()));
            }
            co_return Unexpect{ err };
        }

        // no change
        if (old_stat and not detect_modification(old_stat->get(), *new_stat)) {
            log_d("{}: unchanged: {:?}", __func__, path.fullpath());
            node.expires_after(m_ttl.value_or(Duration::max()));
            co_return Expect<void>{};
        }

        log_w("{}:   changed: {:?}", __func__, path.fullpath());
        co_await m_cache.invalidate_one(node.id(), false);    // maybe conditionally flush?

        switch (new_stat->mode & S_IFMT) {
        case S_IFREG: {
            // TODO: if the file type is unchanged but the data is changed, we need to choose whether to
            // prioritize data from the host or the remote. maybe add a policy on the VFS itself so the user
            // can choose what suits them best.
            node.set_stat(*new_stat);
            node.mutate(node::Regular{});
            node.expires_after(m_ttl.value_or(Duration::max()));
        } break;
        case S_IFDIR: {
            if (S_ISDIR(old_stat->get().mode)) {
                node.set_stat(*new_stat);
                node.set_synced(false);
                node.expires_after(m_ttl.value_or(Duration::max()));
            }
        } break;
        case S_IFLNK: {
            if (auto target = co_await m_connection.readlink(path); target) {
                node.set_stat(*new_stat);
                node.mutate(node::Link{ std::move(target).value() });
            } else {
                node.mutate(node::Error{ target.error() });
            }
            node.expires_after(m_ttl.value_or(Duration::max()));
        } break;
        default: {
            node.set_stat(*new_stat);
            node.mutate(node::Other{});
            node.expires_after(m_ttl.value_or(Duration::max()));
        } break;
        }

        co_return Expect<void>{};
    }

    AExpect<void> FileTree::readdir(path::Path path, Filler filler)
    {
        auto parent = &m_root;

        if (not path.is_root()) {
            auto maybe_node = co_await traverse_or_build(path);
            if (not maybe_node.has_value()) {
                co_return Unexpect{ maybe_node.error() };
            }
            parent = &maybe_node->get();
        }

        auto may_list = parent->list();
        if (not may_list) {
            co_return Unexpect{ may_list.error() };
        }
        auto& list = may_list->get();

        auto pathbuf    = path.extend_copy("dummy").value();
        auto build_file = [&](Str name, mode_t mode) -> Await<File> {
            auto renamed = pathbuf.rename(name);
            assert(renamed);

            switch (mode & S_IFMT) {
            case S_IFREG: co_return node::Regular{};
            case S_IFDIR: co_return node::Directory{};
            case S_IFLNK: {
                if (auto target = co_await m_connection.readlink(pathbuf.as_path()); target) {
                    co_return node::Link{ std::move(target).value() };
                } else {
                    co_return node::Error{ target.error() };
                }
            }
            default: co_return node::Other{};
            }
        };

        if (not parent->has_synced()) {
            auto may_stats = co_await m_connection.statdir(path);
            if (not may_stats) {
                co_return Unexpect{ may_stats.error() };
            }

            if (list.empty()) {
                for (auto [stat, name] : may_stats.value()) {
                    log_d("{}: [{:?}] new entry    : {:?}", __func__, parent->name(), name);

                    auto file  = co_await build_file(name, stat.mode);
                    auto child = std::make_unique<Node>(name, parent, std::move(stat), std::move(file));
                    child->expires_after(m_ttl.value_or(Duration::max()));
                    list.emplace(std::move(child));
                }
            } else {
                auto new_list = std::unordered_set<Str>{};

                // update old entries or add a new one if not exists
                for (auto [stat, name] : may_stats.value()) {
                    new_list.emplace(name);

                    auto found = list.find(name);
                    if (found == list.end()) {
                        log_d("{}: [{:?}] new entry: {:?}", __func__, parent->name(), name);

                        auto file  = co_await build_file(name, stat.mode);
                        auto child = std::make_unique<Node>(name, parent, std::move(stat), std::move(file));
                        child->expires_after(m_ttl.value_or(Duration::max()));
                        list.emplace(std::move(child));

                        continue;
                    }

                    auto& child = (**found);
                    if (auto child_stat = child.stat(); not child_stat) {    // Error node
                        log_d("{}: [{:?}]   changed: {:?}", __func__, parent->name(), name);

                        child.set_stat(std::move(stat));
                        child.mutate(co_await build_file(name, stat.mode));
                        child.expires_after(m_ttl.value_or(Duration::max()));
                    } else if (child.expired() and detect_modification(child_stat->get(), stat)) {
                        log_d("{}: [{:?}]   changed: {:?}", __func__, parent->name(), name);

                        child.set_stat(std::move(stat));
                        child.mutate(co_await build_file(name, stat.mode));
                        child.expires_after(m_ttl.value_or(Duration::max()));

                        co_await m_cache.invalidate_one(child.id(), false);    // should I flush?
                    }

                    log_d("{}: [{:?}] unchanged: {:?}", __func__, parent->name(), name);
                }

                // remove old entries if doesn't exist in new entries
                for (auto it = list.begin(); it != list.end();) {
                    auto name = (**it).name();
                    if (not new_list.contains(name)) {
                        log_d("{}: [{:?}]   removed: {:?}", __func__, parent->name(), name);
                        co_await m_cache.invalidate_one((**it).id(), false);    // should I flush
                        it = list.erase(it);
                        continue;
                    }
                    ++it;
                }
            }

            parent->set_synced(true);
        }

        for (const auto& node : std::as_const(list)) {
            if (not node->as_error()) {
                filler(node->name().data());
            }
        }

        co_return Expect<void>{};
    }

    AExpect<Ref<const data::Stat>> FileTree::getattr(path::Path path)
    {
        co_return (co_await traverse_or_build(path)).and_then(&Node::stat);
    }

    AExpect<Str> FileTree::readlink(path::Path path)
    {
        co_return (co_await traverse_or_build(path)).and_then(&Node::readlink);
    }

    AExpect<Ref<Node>> FileTree::mknod(path::Path path, mode_t mode, dev_t dev)
    {
        auto parent = path.parent_path();
        auto node   = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().mknod(make_context(path), mode, dev);
    }

    AExpect<Ref<Node>> FileTree::mkdir(path::Path path, mode_t mode)
    {
        auto parent = path.parent_path();
        auto node   = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().mkdir(make_context(path), mode);
    }

    AExpect<void> FileTree::unlink(path::Path path)
    {
        auto parent = path.parent_path();
        auto node   = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().unlink(make_context(path));
    }

    AExpect<void> FileTree::rmdir(path::Path path)
    {
        auto parent = path.parent_path();
        auto node   = co_await traverse_or_build(parent);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().rmdir(make_context(path));
    }

    AExpect<void> FileTree::rename(path::Path from, path::Path to, u32 flags)
    {
        // I don't think root can be moved, :P
        if (from.is_root()) {
            co_return Unexpect{ Errc::operation_not_supported };
        }

        auto from_node = co_await traverse_or_build(from);
        if (not from_node.has_value()) {
            co_return Unexpect{ from_node.error() };
        }

        auto to_parent = co_await traverse_or_build(to.parent_path());
        if (not to_parent.has_value()) {
            co_return Unexpect{ to_parent.error() };
        }

        if ((flags & RENAME_EXCHANGE) != 0) {
            auto to_node = co_await traverse_or_build(to);
            if (not to_node.has_value()) {
                co_return Unexpect{ to_node.error() };
            } else if (auto err = to_node->get().as_error(); err != nullptr) {
                co_return Unexpect{ err->error };
            }
        } else if ((flags & RENAME_NOREPLACE) != 0) {
            auto to_node = co_await traverse_or_build(to);
            if (to_node.has_value() and to_node->get().as_error() == nullptr) {
                co_return Unexpect{ Errc::file_exists };
            }
        }

        auto res = co_await m_connection.rename(from, to, flags);
        if (not res) {
            co_return Unexpect{ res.error() };
        }

        auto node = from_node->get().parent()->extract(from.filename()).value();
        co_await m_cache.rename(node->id(), to);

        node->set_name(to.filename());
        node->set_parent(&to_parent->get());
        auto overwritten = to_parent->get().insert(std::move(node), true).value();

        if ((flags & RENAME_EXCHANGE) != 0) {
            assert(overwritten.second != nullptr);
            auto parent = from_node->get().parent();
            auto node   = std::move(overwritten).second;

            co_await m_cache.rename(node->id(), from);
            node->set_name(from.filename());
            node->set_parent(parent);

            auto res = parent->insert(std::move(node), false);
            assert(res->second == nullptr);    // has extracted before
        } else if (overwritten.second != nullptr) {
            co_await m_cache.invalidate_one(overwritten.second->id(), false);
        }

        co_return Expect<void>{};
    }

    AExpect<void> FileTree::truncate(path::Path path, off_t size)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().truncate(make_context(path), size);
    }

    AExpect<u64> FileTree::open(path::Path path, int flags)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().open(make_context(path), flags);
    }

    AExpect<usize> FileTree::read(path::Path path, u64 fd, Span<char> out, off_t offset)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().read(make_context(path), fd, out, offset);
    }

    AExpect<usize> FileTree::write(path::Path path, u64 fd, Str in, off_t offset)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().write(make_context(path), fd, in, offset);
    }

    AExpect<void> FileTree::flush(path::Path path, u64 fd)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().flush(make_context(path), fd);
    }

    AExpect<void> FileTree::release(path::Path path, u64 fd)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().release(make_context(path), fd);
    }

    AExpect<usize> FileTree::copy_file_range(
        path::Path in_path,
        u64        in_fd,
        off_t      in_off,
        path::Path out_path,
        u64        out_fd,
        off_t      out_off,
        size_t     size
    )
    {
        // just in-case they have dirty pages
        std::ignore = co_await flush(in_path, in_fd);
        std::ignore = co_await flush(out_path, out_fd);

        auto node = traverse(out_path);    // path must exist
        if (not node) {
            co_return Unexpect{ node.error() };
        }

        auto copied = co_await m_connection.copy_file_range(in_path, in_off, out_path, out_off, size);
        if (not copied) {
            co_return Unexpect{ copied.error() };
        }

        co_return (co_await m_connection.stat(out_path)).and_then([&](data::Stat new_stat) {
            node->get().set_stat(new_stat);
            return copied;
        });
    }

    AExpect<void> FileTree::utimens(path::Path path, timespec atime, timespec mtime)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        co_return co_await node->get().utimens(make_context(path), atime, mtime);
    }

    Expect<void> FileTree::symlink(path::Path path, Str target)
    {
        return traverse(path.parent_path())
            .and_then(proj(&Node::symlink, path.filename(), target))
            .transform(sink_void);
    }
}
