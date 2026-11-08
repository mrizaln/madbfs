#include "madbfs/filesystem.hpp"

#include "madbfs/connection.hpp"

#include <madbfs-common/log.hpp>

#include <fmt/std.h>
#include <sys/stat.h>

#include <cassert>
#include <unordered_set>

using namespace madbfs;

constexpr auto timespec_now  = timespec{ .tv_sec = 0, .tv_nsec = UTIME_NOW };
constexpr auto timespec_omit = timespec{ .tv_sec = 0, .tv_nsec = UTIME_OMIT };

// helper functions/classes
namespace
{
    Opt<RenameMode> to_rename_mode(u32 flags)
    {
        switch (flags) {
        case 0: return RenameMode::Normal;
        case RENAME_NOREPLACE: return RenameMode::Noreplace;
        case RENAME_EXCHANGE: return RenameMode::Exchange;
        default: return std::nullopt;
        }
    }

    tree::File file_from_mode(mode_t mode)
    {
        switch (mode & S_IFMT) {
        case S_IFREG: return tree::node::Regular{};
        case S_IFDIR: return tree::node::Directory{};
        case S_IFLNK: return tree::node::Link{};
        default: return tree::node::Other{};
        }
    }

    Str mode_name(mode_t mode)
    {
        switch (mode & S_IFMT) {
        case S_IFREG: return "regular";
        case S_IFDIR: return "directory";
        case S_IFLNK: return "symlink";
        default: return "other";
        }
    }
}

// filesystem.hpp impl
namespace madbfs
{
    AExpect<Filesystem> Filesystem::create(
        Connection&      connection,
        cache::LruCache* cache,
        path::Path       root_path
    )
    {
        co_return (co_await connection.stat(root_path)).transform([&](Stat stat) {
            return Filesystem{ connection, cache, root_path, stat };
        });
    }

    Filesystem::Filesystem(
        Connection&      connection,
        cache::LruCache* cache,
        path::Path       root_path,
        Stat             root_stat
    )
        : m_connection{ connection }
        , m_tree{ root_path, root_stat }
        , m_cache{ cache }
    {
    }

    void Filesystem::fill_stbuf(const tree::Node& node, Id id, struct stat* stbuf)
    {
        constexpr auto default_page_size = 64 * 1024;    // use minimum page size

        auto page_size = m_cache ? m_cache->page_size() : default_page_size;
        node.fill_stbuf(stbuf, id, static_cast<blksize_t>(page_size));
    }

    AExpect<void> Filesystem::exchange_nodes(tree::Entry left, tree::Entry right)
    {
        auto&& [id_l, node_l] = left;
        auto&& [id_r, node_r] = right;

        auto name_l = node_l.name();
        auto name_r = node_r.name();

        auto pid_l = node_l.parent();
        auto pid_r = node_r.parent();

        // if the left and right nodes exist, their parents must be as well
        auto& pnode_l = *m_tree.get(pid_l);
        auto& pnode_r = *m_tree.get(pid_r);

        auto& pdir_l = *pnode_l.as_directory();
        auto& pdir_r = *pnode_r.as_directory();

        // since left and right parents are referenced from their respective child, erase will always succeed
        std::ignore = pdir_l.erase(name_l);
        std::ignore = pdir_r.erase(name_r);

        // should I check overwrites? or does FUSE already check this for me?
        std::ignore = pdir_l.insert(name_r, id_r, true);
        std::ignore = pdir_r.insert(name_l, id_l, true);

        node_l.set_parent(pid_r);
        node_r.set_parent(pid_l);

        pnode_l.refresh_stat(timespec_omit, timespec_now);
        pnode_r.refresh_stat(timespec_omit, timespec_now);

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::move_node(tree::Entry left, Str new_name, tree::Entry new_parent)
    {
        auto&& [id_l, node_l]   = left;
        auto&& [pid_r, pnode_r] = new_parent;

        auto  pid_l   = node_l.parent();
        auto& pnode_l = *m_tree.get(pid_l);

        auto& pdir_l = *pnode_l.as_directory();
        auto& pdir_r = *pnode_r.as_directory();

        std::ignore = pdir_l.erase(node_l.name());
        node_l.set_name(new_name);

        // TODO: handle overwrites (Node specified by the Id haven't cleaned from m_nodes)
        std::ignore = pdir_r.insert(node_l.name(), id_l, true);
        node_l.set_parent(pid_r);

        pnode_l.refresh_stat(timespec_omit, timespec_now);
        pnode_r.refresh_stat(timespec_omit, timespec_now);

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::refresh_dir(Id id, path::Path path)
    {
        log_d(__func__, "refresh dir [{}]: {}", id, path);

        if (auto dir = m_tree.get(id).and_then([](tree::Node& n) { return n.as_directory(); }); not dir) {
            co_return Unexpect{ dir.error() };
        }

        // `id` exists and is directory from this point

        auto pathbuf = path.extend_copy("dummy").value();

        auto build_file = [&](Str name, mode_t mode) -> tree::File {
            auto renamed = pathbuf.rename(name);
            assert(renamed);
            return file_from_mode(mode);
        };

        auto may_stats = co_await m_connection.statdir(path);
        if (not may_stats) {
            co_return Unexpect{ may_stats.error() };
        }

        auto stats    = may_stats.value() | sr::to<std::vector>();
        auto new_list = std::unordered_set<Str>{};

        // update old entries or add a new one if not exists
        for (auto [stat, name] : stats) {
            new_list.emplace(name);

            auto found = m_tree.get_child(id, name);

            // new entries
            if (not found) {

                auto file  = build_file(name, stat.mode);
                auto entry = m_tree.create_node(id, name, std::move(stat), std::move(file));

                log_d(__func__, "[{:?}] new entry: [{}] {:?}", path, entry->id, name);

                continue;
            }

            auto&& [child_id, child] = *found;

            if (detect_modification(child.stat(), stat)) {
                log_d(__func__, "[{:?}]   changed: {:?}", path, name);

                auto file = build_file(name, stat.mode);
                child.set_stat(std::move(stat));

                co_await mutate_and_invalidate(child, child_id, std::move(file));
            }

            log_d(__func__, "[{:?}] unchanged: {:?}", path, name);
        }

        auto& dir  = *m_tree.get(id).and_then([](tree::Node& n) { return n.as_directory(); });
        auto& list = dir.children();

        // remove old entries if doesn't exist in new entries
        for (auto it = list.begin(); it != list.end();) {
            const auto& [name, id] = *it;
            if (not new_list.contains(name)) {
                log_d(__func__, "[{:?}]   removed: {:?}", path, name);
                if (m_cache) {
                    co_await m_cache->invalidate_one(id, false);    // should I flush?
                }
                it = list.erase(it);
            } else {
                ++it;
            }
        }

        dir.set_readdir(true);
        co_return Expect<void>{};
    }

    Await<void> Filesystem::mutate_and_invalidate(tree::Node& node, Id id, tree::File file)
    {
        auto old = node.mutate(std::move(file));

        if (auto dir = std::get_if<tree::node::Directory>(&old)) {
            auto ids = std::vector<Id>{};
            for (auto&& [_, id] : dir->children()) {
                walk(id, [&](tree::Entry e) { m_handles.erase(e.id), ids.push_back(e.id); });
            }
            if (m_cache) {
                for (auto id : ids) {
                    co_await m_cache->invalidate_one(id, false);    // flush? child maybe unchanged
                }
            }
            m_handles.erase(id);
        } else if (std::get_if<tree::node::Regular>(&old)) {
            if (m_cache) {
                co_await m_cache->invalidate_one(id, false);    // no flush since the file changed
            }
            m_handles.erase(id);
        }
    }

    usize Filesystem::walk(Id start, std::function<void(tree::Entry)> func)
    {
        auto stack = Vec<Id>{ start };
        auto count = 0uz;

        while (not stack.empty()) {
            auto id = stack.back();
            stack.pop_back();

            if (auto node = m_tree.get(id); node) {
                func({ id, *node });
                ++count;

                if (auto dir = node->as_directory(); dir) {
                    for (auto&& [_, id] : dir->children()) {
                        stack.push_back(id);
                    }
                }
            }
        }

        return count;
    }

    AExpect<void> Filesystem::ll_lookup(Id parent, Str name, struct stat* stbuf)
    {
        // check if it has been looked up
        if (auto entry = m_tree.get_child(parent, name); entry) {
            fill_stbuf(entry->node, entry->id, stbuf);
            entry->node.incr_lookup(1);

            log_d(__func__, "looked-up from tree: [{}] {}", entry->id, name);

            co_return Expect<void>{};
        }

        auto path = m_tree.build_path(parent, name);
        if (not path) {
            log_c(__func__, "failed to build path: {}", name);
            co_return Unexpect{ path.error() };
        }

        co_return (co_await m_connection.stat(*path)).transform([&](Stat stat) {
            auto file  = file_from_mode(stat.mode);
            auto entry = m_tree.create_node(parent, name, stat, file, true);

            fill_stbuf(entry->node, entry->id, stbuf);

            log_d("ll_lookup", "lookup out of tree: [{}] {}", entry->id, *path);
        });
    }

    AExpect<void> Filesystem::ll_forget(Id id, u64 nlookup)
    {
        auto remove = false;

        if (auto node = m_tree.get(id); node) {
            auto current = node->decr_lookup(nlookup);
            remove       = current == 0;
        }

        if (not remove) {
            co_return Expect<void>{};
        }

        // node must exist since `remove` can only be true if above branch is executed
        auto node = m_tree.remove(id).value();

        auto unlinked = m_orphans.remove(node.parent(), node.name());
        if (not unlinked) {
            co_return Expect<void>{};
        }

        if (auto reg = node.as_regular(); reg) {
            if (reg->open_count > 0) [[unlikely]] {
                m_handles.erase(id);
            }
            if (m_cache) {
                co_await m_cache->invalidate_one(id, true);
            }
        }

        auto path = m_tree.build_path(node.parent(), *unlinked);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        co_return co_await m_connection.unlink(*path);
    }

    AExpect<void> Filesystem::ll_getattr(Id id, struct stat* stbuf)
    {
        // TODO: check for modification
        co_return m_tree.get(id).transform([&](tree::Node& node) { fill_stbuf(node, id, stbuf); });
    }

    AExpect<void> Filesystem::ll_setattr(
        Id            id,
        Opt<u64>      size,
        Opt<timespec> atime,
        Opt<timespec> mtime,
        struct stat*  stbuf
    )
    {
        auto node = m_tree.get(id);
        if (not node) {
            co_return Unexpect{ node.error() };
        }

        auto path = m_tree.build_path(id);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        if (size) {
            if (auto file = node->as_regular(); not file) {
                co_return Unexpect{ file.error() };
            }

            if (m_cache) {
                std::ignore = co_await m_cache->flush(id);
            }

            if (auto res = co_await m_connection.truncate(*path, static_cast<off_t>(*size)); not res) {
                co_return Unexpect{ res.error() };
            }

            auto old_size = static_cast<usize>(node->stat().size);
            auto new_size = static_cast<usize>(*size);

            // error from Cache::truncate are from eviction only, which should not matter for this operation
            if (m_cache) {
                std::ignore = co_await m_cache->truncate(id, old_size, new_size);
            }

            node->set_size(static_cast<off_t>(*size));
        }

        if (atime or mtime) {
            auto atime_real = atime.value_or(timespec_omit);
            auto mtime_real = mtime.value_or(timespec_omit);

            if (auto res = co_await m_connection.utimens(*path, atime_real, mtime_real); not res) {
                co_return Unexpect{ res.error() };
            }

            if (auto res = co_await m_connection.stat(*path); not res) {
                co_return Unexpect{ res.error() };
            } else {
                node->set_stat(std::move(*res));
            }
        }

        log_d(__func__, "fill stbuf");
        fill_stbuf(*node, id, stbuf);
        co_return Expect<void>{};
    }

    AExpect<Str> Filesystem::ll_readlink(Id id)
    {
        auto node = m_tree.get(id);
        if (not node) {
            co_return Unexpect{ node.error() };
        }

        auto path = m_tree.build_path(id);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        auto link = node->as_link();
        if (not link) {
            co_return Unexpect{ link.error() };
        }

        if (link->target) {
            co_return Str{ link->target.value() };
        }

        auto target = co_await m_connection.readlink(*path);
        if (not target) {
            co_return Unexpect{ target.error() };
        }

        link->target = std::move(target).value();
        co_return Str{ link->target.value() };
    }

    AExpect<void> Filesystem::ll_mknod(Id parent, Str name, mode_t mode, dev_t dev, struct stat* stbuf)
    {
        auto parent_node = m_tree.get(parent);
        if (not parent_node) {
            co_return Unexpect{ parent_node.error() };
        }

        auto path = m_tree.build_path(parent, name);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        auto dir = parent_node->as_directory();
        if (not dir) {
            co_return Unexpect{ dir.error() };
        }

        auto overwrite = false;
        if (auto id = dir->find(name); id) {
            auto node = m_tree.get(*id);
            if (not node) {
                co_return Unexpect{ Errc::no_such_file_or_directory };
            }
            overwrite = true;
        }

        if (auto created = co_await m_connection.mknod(*path, mode, dev); not created) {
            parent_node->refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ created.error() };
        }

        co_return (co_await m_connection.stat(*path))
            .and_then([&](Stat stat) {
                return m_tree.create_node(parent, name, std::move(stat), tree::node::Regular{}, true);
            })
            .transform([&](tree::Entry entry) { fill_stbuf(entry.node, entry.id, stbuf); });
    }

    AExpect<void> Filesystem::ll_mkdir(Id parent, Str name, mode_t mode, struct stat* stbuf)
    {
        auto parent_node = m_tree.get(parent);
        if (not parent_node) {
            co_return Unexpect{ parent_node.error() };
        }

        auto path = m_tree.build_path(parent, name);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        auto dir = parent_node->as_directory();
        if (not dir) {
            co_return Unexpect{ dir.error() };
        }

        auto overwrite = false;
        if (auto id = dir->find(name); id) {
            if (auto node = m_tree.get(*id); not node) {
                co_return Unexpect{ Errc::no_such_file_or_directory };
            }
            overwrite = true;
        }

        if (auto created = co_await m_connection.mkdir(*path, mode); not created) {
            parent_node->refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ created.error() };
        }

        co_return (co_await m_connection.stat(*path))
            .and_then([&](Stat stat) {
                return m_tree.create_node(parent, name, std::move(stat), tree::node::Directory{}, true);
            })
            .transform([&](tree::Entry entry) { fill_stbuf(entry.node, entry.id, stbuf); });
    }

    AExpect<void> Filesystem::ll_unlink(Id parent, Str name)
    {
        auto child = m_tree.get_child(parent, name);
        if (not child) {
            co_return Unexpect{ child.error() };
        }

        auto& pnode = *m_tree.get(parent);
        auto& dir   = *pnode.as_directory();

        if (child->node.is_directory()) {
            co_return Unexpect{ Errc::is_a_directory };
        }

        auto path = m_tree.build_path(child->id);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        if (auto reg = child->node.as_regular(); reg and reg->open_count > 0) {
            auto orphan_name = m_orphans.add(parent, name);
            auto orphan_path = path.value();
            auto renamed     = orphan_path.rename(orphan_name);
            assert(renamed);

            if (auto res = co_await m_connection.rename(*path, orphan_path, 0); not res) {
                m_orphans.remove(parent, name);
                co_return Unexpect{ res.error() };
            }

            co_await m_cache->rename(child->id, orphan_path);
        } else {
            if (auto res = co_await m_connection.unlink(*path); not res) {
                co_return Unexpect{ res.error() };
            }

            m_tree.remove(child->id);
        }

        pnode.refresh_stat(timespec_omit, timespec_now);
        auto erased = dir.erase(name);
        assert(erased.has_value());

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::ll_rmdir(Id parent, Str name)
    {
        auto child = m_tree.get_child(parent, name);
        if (not child) {
            co_return Unexpect{ child.error() };
        }

        auto& pnode = *m_tree.get(parent);
        auto& dir   = *pnode.as_directory();

        auto target = child->node.as_directory();
        if (not target) {
            co_return Unexpect{ target.error() };
        }

        if (not target->children().empty()) {
            co_return Unexpect{ Errc::directory_not_empty };
        }

        auto path = m_tree.build_path(child->id);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        co_return (co_await m_connection.rmdir(*path)).transform([&] {
            pnode.refresh_stat(timespec_omit, timespec_now);
            std::ignore = m_tree.remove(child->id);
            std::ignore = dir.erase(name);
        });
    }

    AExpect<void> Filesystem::ll_rename(Id parent, Str name, Id new_parent, Str new_name, u32 flags)
    {
        auto mode = to_rename_mode(flags);
        if (not mode) {
            co_return Unexpect{ Errc::invalid_argument };
        }

        auto from_entry = m_tree.get_child(parent, name);
        if (not from_entry) {
            co_return Unexpect{ from_entry.error() };
        }

        auto from_path = m_tree.build_path(from_entry->id);
        if (not from_path) {
            co_return Unexpect{ from_path.error() };
        }

        log_d(__func__, "rename entry [{}]: {}", from_entry->id, from_entry->node.name());

        switch (*mode) {
        case RenameMode::Noreplace: {
            if (auto to_entry = m_tree.get_child(new_parent, new_name); to_entry) {
                co_return Unexpect{ Errc::file_exists };
            }

            auto to_path = m_tree.build_path(new_parent, new_name);
            if (not to_path) {
                co_return Unexpect{ to_path.error() };
            }

            if (auto res = co_await m_connection.rename(*from_path, *to_path, flags); not res) {
                co_return Unexpect{ res.error() };
            }
            if (m_cache) {
                co_await m_cache->rename(from_entry->id, *to_path);
            }

            // parent must exist at this point
            auto to_parent = tree::Entry{
                .id   = new_parent,
                .node = *m_tree.get(new_parent),
            };

            co_return co_await move_node(*from_entry, new_name, to_parent);
        } break;
        case RenameMode::Exchange: {
            auto to_entry = m_tree.get_child(new_parent, new_name);
            if (not to_entry) {
                co_return Unexpect{ to_entry.error() };
            }

            auto to_path = m_tree.build_path(new_parent, new_name);
            if (not to_path) {
                co_return Unexpect{ to_path.error() };
            }

            if (auto res = co_await m_connection.rename(*from_path, *to_path, flags); not res) {
                co_return Unexpect{ res.error() };
            }
            if (m_cache) {
                co_await m_cache->rename(from_entry->id, *to_path);
            }

            co_return co_await exchange_nodes(*from_entry, *to_entry);
        } break;
        case RenameMode::Normal: [[fallthrough]];
        default: {
            auto to_parent = m_tree.get(new_parent);
            if (not to_parent) {
                co_return Unexpect{ to_parent.error() };
            }
            if (not to_parent->is_directory()) {
                co_return Unexpect{ Errc::not_a_directory };
            }

            auto to_path = m_tree.build_path(new_parent, new_name);
            if (not to_path) {
                co_return Unexpect{ to_path.error() };
            }

            if (auto res = co_await m_connection.rename(*from_path, *to_path, flags); not res) {
                co_return Unexpect{ res.error() };
            }
            if (m_cache) {
                co_await m_cache->rename(from_entry->id, *to_path);
            }

            co_return co_await move_node(*from_entry, new_name, { new_parent, *to_parent });
        }
        }
    }

    AExpect<u64> Filesystem::ll_open(Id id, i32 flags)
    {
        auto node = m_tree.get(id);
        if (not node) {
            co_return Unexpect{ node.error() };
        }

        auto file = node->as_regular();
        if (not file) {
            co_return Unexpect{ file.error() };
        }

        auto mode = static_cast<OpenMode>(O_ACCMODE & flags);

        auto path = m_tree.build_path(id);
        if (not path) {
            co_return Unexpect{ path.error() };
        }

        ++file->open_count;

        // send hint to cache to prepare a real fd that can be used for further operations
        if (m_cache) {
            co_return (co_await m_cache->hint_open(id, *path, mode)).transform([&] {
                return m_handles.store(id, mode, 0);
            });
        } else {
            co_return (co_await m_connection.open(*path, mode)).transform([&](u64 real_fd) {
                return m_handles.store(id, mode, real_fd);
            });
        }
    }

    AExpect<usize> Filesystem::ll_read(u64 fd, Span<char> out, off_t offset)
    {
        auto handle = m_handles.find(fd, OpenMode::Read);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto after = [&](usize ret) {
            node->refresh_stat(timespec_now, timespec_omit);
            return ret;
        };

        if (m_cache) {
            co_return (co_await m_cache->read(handle->id, out, offset)).transform(after);
        } else {
            assert(handle->real_fd != 0 && "on no-cache, the file descriptor is exposed directly, not 0");
            co_return (co_await m_connection.read(handle->real_fd, out, offset)).transform(after);
        }
    }

    AExpect<usize> Filesystem::ll_write(u64 fd, Span<const char> in, off_t offset)
    {
        auto handle = m_handles.find(fd, OpenMode::Write);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto file = node->as_regular();
        if (not file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto after = [&](usize ret) {
            // the file size is defined as offset + size from last write if it's higher than previous size
            auto new_size = offset + static_cast<off_t>(ret);
            auto size     = std::max(node->stat().size, new_size);

            node->set_size(size);
            node->refresh_stat(timespec_omit, timespec_now);

            file->dirty = true;

            return ret;
        };

        if (m_cache) {
            co_return (co_await m_cache->write(handle->id, in, offset)).transform(after);
        } else {
            co_return (co_await m_connection.write(handle->real_fd, in, offset)).transform(after);
        }
    }

    AExpect<void> Filesystem::ll_release(u64 fd)
    {
        auto handle = m_handles.release(fd);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto file = node->as_regular();
        if (not file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        if (file->dirty and m_cache) {
            if (auto res = co_await m_cache->flush(handle->id); not res) {
                co_return Unexpect{ res.error() };
            }

            node->refresh_stat(timespec_omit, timespec_now);
            file->dirty = false;
        }

        if (file->open_count > 0) {
            --file->open_count;
        }

        // send hint to cache to close its associated fd for this node if exist
        if (m_cache) {
            co_return co_await m_cache->hint_close(handle->id, handle->mode);
        } else {
            co_return co_await m_connection.close(handle->real_fd);
        }
    }

    AExpect<void> Filesystem::ll_fsync(u64 fd, bool data_only)
    {
        auto handle = m_handles.find(fd);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto file = node->as_regular();
        if (not file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        if (not file->dirty) {
            co_return Expect<void>{};    // no writes, do nothing
        }

        if (m_cache) {
            co_return (co_await m_cache->flush(handle->id)).transform([&] {
                node->refresh_stat(timespec_omit, timespec_now);
                file->dirty = false;
            });
        } else {
            // do nothing, write already happens on write :P
            file->dirty = false;
            co_return Expect<void>{};
        }
    }

    AExpect<void> Filesystem::ll_readdir(Id id, off_t offset, Filler filler)
    {
        auto node = m_tree.get(id);
        if (not node) {
            co_return Unexpect{ node.error() };
        }

        auto dir = node->as_directory();
        if (not dir) {
            co_return Unexpect{ dir.error() };
        }

        auto path = m_tree.build_path(id);
        if (not path) {
            co_return Unexpect{ dir.error() };
        }

        if (not dir->has_readdir()) {
            if (auto res = co_await refresh_dir(id, *path); not res) {
                co_return Unexpect{ res.error() };
            }
        }

        struct stat stat;

        auto& children = dir->children();

        for (auto&& [i, child] : std::as_const(children) | sv::enumerate | sv::drop(offset)) {
            auto node = m_tree.get(child.second);
            if (not node) {
                continue;
            }

            fill_stbuf(*node, child.second, &stat);
            if (auto full = filler(node->name().data(), &stat, i + 1); full) {
                break;
            }

            node->incr_lookup(1);
        }

        co_return Expect<void>{};
    }

    AExpect<usize> Filesystem::ll_copy_file_range(
        Id     in_id,
        u64    in_fd,
        off_t  in_off,
        Id     out_id,
        u64    out_fd,
        off_t  out_off,
        size_t size
    )
    {
        // just in-case they have dirty pages
        std::ignore = co_await ll_fsync(in_fd, true);
        std::ignore = co_await ll_fsync(out_fd, true);

        auto in_node = m_tree.get(in_id);
        if (not in_node) {
            co_return Unexpect{ in_node.error() };
        }

        auto in_path = m_tree.build_path(in_id);
        if (not in_path) {
            co_return Unexpect{ in_path.error() };
        }

        auto out_node = m_tree.get(out_id);
        if (not out_node) {
            co_return Unexpect{ out_node.error() };
        }

        auto out_path = m_tree.build_path(out_id);
        if (not out_path) {
            co_return Unexpect{ out_path.error() };
        }

        auto copied = co_await m_connection.copy_file_range(*in_path, in_off, *out_path, out_off, size);
        if (not copied) {
            co_return Unexpect{ copied.error() };
        }

        co_return (co_await m_connection.stat(*out_path)).and_then([&](Stat new_stat) {
            in_node->refresh_stat(timespec_now, timespec_omit);
            out_node->set_stat(new_stat);
            return copied;
        });
    }
}
