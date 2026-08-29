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
    // NOTE: Errc::not_connected, Errc::timed_out, and Errc::resource_unavailable_try_again should be
    // considered an OK error since it can happen if the device is disconnected. The error should not be
    // cached as node.
    bool should_cache_error(Errc err)
    {
        return err != std::errc::not_connected    //
           and err != std::errc::timed_out        //
           and err != std::errc::resource_unavailable_try_again;
    }

    Opt<cache::LruCache> construct_cache(async::Context& ctx, Connection& connection, Opt<Caching> caching)
    {
        return caching.transform([&](auto c) mutable {
            return async::block(ctx, cache::LruCache::create(connection, c.page_size, c.max_pages));
        });
    }

    blksize_t page_size_or_default(Opt<cache::LruCache>& cache)
    {
        constexpr auto default_page_size = 64 * 1024;    // use minimum page size

        auto page_size = cache.transform(&cache::LruCache::page_size).value_or(default_page_size);
        return static_cast<blksize_t>(page_size);
    }

    Opt<RenameMode> to_rename_mode(u32 flags)
    {
        switch (flags) {
        case 0: return RenameMode::Normal;
        case RENAME_NOREPLACE: return RenameMode::Noreplace;
        case RENAME_EXCHANGE: return RenameMode::Exchange;
        default: return std::nullopt;
        }
    }
}

// filesystem.hpp impl
namespace madbfs
{
    Filesystem::Filesystem(
        async::Context& ctx,
        Connection&     connection,
        Opt<Caching>    caching,
        Opt<Seconds>    ttl
    )
        : m_connection{ connection }
        , m_tree{ "/", Stat{} }    // dummy
        , m_cache{ construct_cache(ctx, connection, caching) }
        , m_ttl{ ttl }
    {
    }

    AExpect<Id> Filesystem::build(Id parent_id, path::Path path)
    {
        const auto name = path.filename();

        auto build_then_expire = [&](Str name, Stat stat, tree::File file) {
            auto [id, node] = m_tree.create_node(parent_id, name, stat, file).value();
            node.expires_after(m_ttl.value_or(Seconds::max()));
            return id;
        };

        auto stat = co_await m_connection.stat(path);
        if (not stat.has_value()) {
            auto err = stat.error();
            if (should_cache_error(err)) {
                std::ignore = build_then_expire(name, {}, tree::node::Error{ err });
            }
            co_return Unexpect{ err };
        }

        switch (stat->mode & S_IFMT) {
        case S_IFREG: co_return build_then_expire(name, *stat, tree::node::Regular{});
        case S_IFDIR: co_return build_then_expire(name, *stat, tree::node::Directory{});
        case S_IFLNK: co_return build_then_expire(name, *stat, tree::node::Link{});
        default: co_return build_then_expire(name, *stat, tree::node::Other{});
        }
    }

    AExpect<Id> Filesystem::build_directory(Id parent_id, path::Path path)
    {
        const auto name = path.filename();

        auto build_then_expire = [&](Str name, Stat stat, tree::File file) {
            auto [id, node] = m_tree.create_node(parent_id, name, stat, file).value();
            node.expires_after(m_ttl.value_or(Seconds::max()));
            return id;
        };

        auto stat = co_await m_connection.stat(path);
        if (not stat.has_value()) {
            auto err = stat.error();
            if (should_cache_error(err)) {
                std::ignore = build_then_expire(name, {}, tree::node::Error{ err });
            }
            co_return Unexpect{ err };
        } else if ((stat->mode & S_IFMT) != S_IFDIR) {
            co_return Unexpect{ Errc::not_a_directory };
        }

        co_return build_then_expire(name, *stat, tree::node::Directory{});
    }

    Expect<Id> Filesystem::traverse(path::Path path)
    {
        if (path.is_root()) {
            return m_tree.root();
        }

        auto current = m_tree.root();

        for (auto name : path.iter()) {
            auto node = m_tree.get(current);
            if (not node) {
                return Unexpect{ Errc::no_such_file_or_directory };
            }

            auto next = node->get().traverse(name);
            if (not next) {
                return Unexpect{ next.error() };
            }

            current = *next;
        }

        return current;
    }

    Expect<tree::Entry> Filesystem::traverse_entry(path::Path path)
    {
        auto id = traverse(path);
        if (not id) {
            return Unexpect{ id.error() };
        }
        auto node = m_tree.get(*id);
        if (not node) {
            return Unexpect{ Errc::no_such_file_or_directory };
        }
        return tree::Entry{ .id = *id, .node = *node };
    }

    AExpect<Id> Filesystem::traverse_or_build(path::Path path)
    {
        if (path.is_root()) {
            co_return m_tree.root();
        }

        auto current      = m_tree.root();
        auto current_path = path::PathBuf{};

        for (auto [i, name] : path.iter() | sv::enumerate) {
            auto node = m_tree.get(current);
            if (not node) {
                co_return Unexpect{ Errc::no_such_file_or_directory };
            } else if (node->get().expired()) {
                if (auto res = co_await update(*node, current, current_path); not res) {
                    co_return Unexpect{ res.error() };
                }
            }

            auto next = node->get().traverse(name);
            current_path.extend(name);

            if (not next) {
                if (static_cast<usize>(i) < path.depth() - 1) {
                    next = co_await build_directory(current, current_path);
                } else {
                    next = co_await build(current, current_path);
                }
            }

            if (not next) {
                co_return Unexpect{ next.error() };
            }
            current = *next;
        }

        co_return current;
    }

    AExpect<tree::Entry> Filesystem::traverse_or_build_entry(path::Path path)
    {
        auto id = co_await traverse_or_build(path);
        if (not id) {
            co_return Unexpect{ id.error() };
        }
        auto node = m_tree.get(*id);
        if (not node) {
            co_return Unexpect{ Errc::no_such_file_or_directory };
        }
        co_return tree::Entry{ .id = *id, .node = *node };
    }

    AExpect<void> Filesystem::update(tree::Node& node, Id id, path::Path path)
    {
        log_d(__func__, "{:?}", path);

        auto new_stat = co_await m_connection.stat(path);
        auto old_stat = node.stat();

        if (not new_stat) {
            auto err = new_stat.error();
            if (should_cache_error(err)) {
                co_await mutate_and_invalidate(node, id, tree::node::Error{ err });
                node.expires_after(m_ttl.value_or(Seconds::max()));
            }
            co_return Unexpect{ err };
        }

        // no change
        if (not node.is_error() and not detect_modification(old_stat, *new_stat)) {
            log_d(__func__, "unchanged: {:?}", path);
            node.expires_after(m_ttl.value_or(Seconds::max()));
            co_return Expect<void>{};
        }

        log_w(__func__, "  changed: {:?}", path);

        switch (new_stat->mode & S_IFMT) {
        case S_IFREG: {
            node.set_stat(*new_stat);
            co_await mutate_and_invalidate(node, id, tree::node::Regular{});    // invalidate current data
            node.expires_after(m_ttl.value_or(Seconds::max()));
        } break;
        case S_IFDIR: {
            if (S_ISDIR(old_stat.mode)) {    // previously directory
                node.set_stat(*new_stat);
                node.set_synced(false);    // don't mutate, force rescan
                node.expires_after(m_ttl.value_or(Seconds::max()));
            } else {
                node.set_stat(*new_stat);
                co_await mutate_and_invalidate(node, id, tree::node::Directory{});    // not dir, become dir
                node.expires_after(m_ttl.value_or(Seconds::max()));
            }
        } break;
        case S_IFLNK: {
            node.set_stat(*new_stat);
            co_await mutate_and_invalidate(node, id, tree::node::Link{});
            node.expires_after(m_ttl.value_or(Seconds::max()));
        } break;
        default: {
            node.set_stat(*new_stat);
            co_await mutate_and_invalidate(node, id, tree::node::Other{});
            node.expires_after(m_ttl.value_or(Seconds::max()));
        } break;
        }

        co_return Expect<void>{};
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
        auto& pnode_l = m_tree.get(pid_l)->get();
        auto& pnode_r = m_tree.get(pid_r)->get();

        auto& pdir_l = pnode_l.as_directory()->get();
        auto& pdir_r = pnode_r.as_directory()->get();

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
        auto& pnode_l = m_tree.get(pid_l)->get();

        auto& pdir_l = pnode_l.as_directory()->get();
        auto& pdir_r = pnode_r.as_directory()->get();

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
        log_d(__func__, "refresh dir [{}]: {}", id.to_u64(), path);

        if (auto dir = m_tree.get(id).and_then([](tree::Node& n) { return n.as_directory(); }); not dir) {
            co_return Unexpect{ dir.error() };
        }

        // `id` exists and is directory from this point

        auto pathbuf = path.extend_copy("dummy").value();

        auto build_file = [&](Str name, mode_t mode) -> tree::File {
            auto renamed = pathbuf.rename(name);
            assert(renamed);

            switch (mode & S_IFMT) {
            case S_IFREG: return tree::node::Regular{};
            case S_IFDIR: return tree::node::Directory{};
            case S_IFLNK: return tree::node::Link{};
            default: return tree::node::Other{};
            }
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
                log_d(__func__, "[{:?}] new entry: {:?}", path, name);

                auto file  = build_file(name, stat.mode);
                auto entry = m_tree.create_node(id, name, std::move(stat), std::move(file)).value();

                entry.node.expires_after(m_ttl.value_or(Seconds::max()));

                continue;
            }

            auto&& [child_id, child] = *found;

            if (child.is_error()) {    // Error node
                log_d(__func__, "[{:?}]   changed: {:?}", path, name);

                auto file = build_file(name, stat.mode);
                child.set_stat(std::move(stat));

                co_await mutate_and_invalidate(child, child_id, std::move(file));
                child.expires_after(m_ttl.value_or(Seconds::max()));

            } else if (child.expired() and detect_modification(child.stat(), stat)) {
                log_d(__func__, "[{:?}]   changed: {:?}", path, name);

                auto file = build_file(name, stat.mode);
                child.set_stat(std::move(stat));

                co_await mutate_and_invalidate(child, child_id, std::move(file));
                child.expires_after(m_ttl.value_or(Seconds::max()));
            }

            log_d(__func__, "[{:?}] unchanged: {:?}", path, name);
        }

        auto& dir  = m_tree.get(id).and_then([](tree::Node& n) { return n.as_directory(); })->get();
        auto& list = dir.children();

        // remove old entries if doesn't exist in new entries
        for (auto it = list.begin(); it != list.end();) {
            const auto& [name, id] = *it;
            if (not new_list.contains(name)) {
                log_d(__func__, "[{:?}]   removed: {:?}", path, name);
                if (m_cache) {
                    co_await m_cache->invalidate_one(id, false);    // should I flush
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

    void Filesystem::walk(Id start, std::function<void(tree::Entry)> func)
    {
        auto stack = Vec<Id>{ start };

        while (not stack.empty()) {
            auto id = stack.back();
            stack.pop_back();

            if (auto node = m_tree.get(id); node) {
                func({ id, *node });

                if (auto dir = node->get().as_directory(); dir) {
                    for (auto&& [_, id] : dir->get().children()) {
                        stack.push_back(id);
                    }
                }
            }
        }
    }

    AExpect<void> Filesystem::readdir(path::Path path, Filler filler, off_t offset)
    {
        auto entry = co_await traverse_or_build_entry(path);
        if (not entry) {
            co_return Unexpect{ entry.error() };
        }

        auto [id, node] = *entry;

        auto dir = node.as_directory();
        if (not dir) {
            co_return Unexpect{ dir.error() };
        }

        if (not dir->get().has_readdir()) {
            if (auto res = co_await refresh_dir(id, path); not res) {
                co_return Unexpect{ res.error() };
            }
        }

        struct stat stat;

        auto& children = dir->get().children();

        for (auto&& [i, child] : std::as_const(children) | sv::enumerate | sv::drop(offset)) {
            auto node = m_tree.get(child.second);
            if (not node or node->get().is_error()) {
                continue;
            }

            node->get().fill_stbuf(&stat, child.second, page_size_or_default(m_cache));
            if (auto full = filler(node->get().name().data(), &stat, i + 1); full) {
                break;
            }
        }

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::getattr(path::Path path, struct stat* stbuf)
    {
        co_return (co_await traverse_or_build_entry(path)).and_then([&](tree::Entry entry) -> Expect<void> {
            auto [id, node] = entry;
            if (auto err = node.as_error(); err) {
                return Unexpect{ err->error };
            }
            node.fill_stbuf(stbuf, id, page_size_or_default(m_cache));
            return Expect<void>{};
        });
    }

    AExpect<Str> Filesystem::readlink(path::Path path)
    {
        auto entry = co_await traverse_or_build_entry(path);
        if (not entry) {
            co_return Unexpect{ entry.error() };
        }

        auto [id, node] = *entry;

        auto link = node.as_link();
        if (not link) {
            co_return Unexpect{ link.error() };
        }

        if (link->get().target) {
            co_return Str{ link->get().target.value() };
        }

        auto target = co_await m_connection.readlink(path);
        if (not target) {
            co_return Unexpect{ target.error() };
        }

        link->get().target = std::move(target).value();
        co_return Str{ link->get().target.value() };
    }

    AExpect<Id> Filesystem::mknod(path::Path path, mode_t mode, dev_t dev)
    {
        auto entry = co_await traverse_or_build_entry(path.parent_path());
        if (not entry) {
            co_return Unexpect{ entry.error() };
        }

        auto [id, node] = *entry;

        auto may_dir = node.as_directory();
        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir       = may_dir->get();
        auto  name      = path.filename();
        auto  overwrite = false;

        if (auto id = dir.find(name); id) {
            auto node = m_tree.get(*id);
            if (not node) {
                co_return Unexpect{ Errc::no_such_file_or_directory };
            } else if (not node->get().is_error()) {
                co_return Unexpect{ Errc::file_exists };
            }
            overwrite = true;
        }

        if (auto created = co_await m_connection.mknod(path, mode, dev); not created) {
            node.refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ created.error() };
        }

        co_return (co_await m_connection.stat(path))
            .and_then([&](Stat stat) {
                return m_tree.create_node(id, name, std::move(stat), tree::node::Regular{}, true);
            })
            .transform([&](tree::Entry entry) { return entry.id; });
    }

    AExpect<Id> Filesystem::mkdir(path::Path path, mode_t mode)
    {
        auto parent_entry = co_await traverse_or_build_entry(path.parent_path());
        if (not parent_entry) {
            co_return Unexpect{ parent_entry.error() };
        }

        auto [id, parent] = *parent_entry;

        auto may_dir = parent.as_directory();
        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir       = may_dir->get();
        auto  name      = path.filename();
        auto  overwrite = false;

        if (auto id = dir.find(name); id) {
            if (auto node = m_tree.get(*id); not node) {
                co_return Unexpect{ Errc::no_such_file_or_directory };
            } else if (not node->get().is_error()) {
                co_return Unexpect{ Errc::file_exists };
            }
            overwrite = true;
        }

        if (auto created = co_await m_connection.mkdir(path, mode); not created) {
            parent.refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ created.error() };
        }

        co_return (co_await m_connection.stat(path))
            .and_then([&](Stat stat) {
                return m_tree.create_node(id, name, std::move(stat), tree::node::Directory{}, true);
            })
            .transform([&](tree::Entry entry) { return entry.id; });
    }

    AExpect<void> Filesystem::unlink(path::Path path)
    {
        auto name         = path.filename();
        auto parent_child = (co_await traverse_or_build(path.parent_path())).and_then([&](Id id) {
            return m_tree.get_child(id, name).transform([&](tree::Entry e) { return std::pair{ id, e }; });
        });

        if (not parent_child) {
            co_return Unexpect{ parent_child.error() };
        }

        auto [parent_id, child] = *parent_child;

        auto& dir = m_tree.get(parent_id)->get().as_directory()->get();

        if (child.node.is_error()) {
            co_return Unexpect{ child.node.as_error()->error };
        } else if (child.node.is_directory()) {
            co_return Unexpect{ Errc::is_a_directory };
        }

        auto erased = dir.erase(path.filename());
        assert(erased.has_value());

        log_d(__func__, "erased {}: {}", erased->to_u64(), m_tree.get(*erased)->get().name());

        if (auto res = co_await m_connection.unlink(path); not res) {
            auto overwritten = dir.insert(name, *erased, true);    // re-insert on failure :P
            assert(not overwritten.has_value());

            child.node.refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ res.error() };
        }

        if (m_cache) {
            co_await m_cache->invalidate_one(*erased, false);
        }

        m_tree.remove(child.id);
        m_handles.erase(*erased);

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::rmdir(path::Path path)
    {
        auto name         = path.filename();
        auto parent_child = (co_await traverse_or_build(path.parent_path())).and_then([&](Id id) {
            return m_tree.get_child(id, name).transform([&](tree::Entry e) { return std::pair{ id, e }; });
        });

        if (not parent_child) {
            co_return Unexpect{ parent_child.error() };
        }

        auto [parent_id, child] = *parent_child;

        auto& parent = m_tree.get(parent_id)->get();
        auto& dir    = parent.as_directory()->get();

        if (child.node.is_error()) {
            co_return Unexpect{ child.node.as_error()->error };
        } else if (not child.node.is_directory()) {
            co_return Unexpect{ Errc::not_a_directory };
        }

        auto target = child.node.as_directory();
        if (not target) {
            co_return Unexpect{ target.error() };
        }

        // disallow erasing if all the children is not Error
        if (const auto& children = target->get().children(); not children.empty()) {
            auto is_error = [&](Id id) {
                auto node = m_tree.get(id);
                return node ? node->get().is_error() : true;
            };

            if (not sr::all_of(children | sv::values, is_error)) {
                co_return Unexpect{ Errc::directory_not_empty };
            }
        }

        co_return (co_await m_connection.rmdir(path)).transform([&] {
            parent.refresh_stat(timespec_omit, timespec_now);
            m_tree.remove(child.id);
            std::ignore = dir.erase(name);
        });
    }

    AExpect<void> Filesystem::rename(path::Path from, path::Path to, u32 flags)
    {
        auto mode = to_rename_mode(flags);
        if (not mode) {
            co_return Unexpect{ Errc::invalid_argument };
        }

        auto from_entry = co_await traverse_or_build_entry(from);
        if (not from_entry) {
            co_return Unexpect{ from_entry.error() };
        }

        log_d(__func__, "rename entry [{}]: {}", from_entry->id.to_u64(), from_entry->node.name());

        switch (*mode) {
        case RenameMode::Noreplace: {
            auto to_entry = co_await traverse_or_build_entry(to);
            if (to_entry and not to_entry->node.is_error()) {
                co_return Unexpect{ Errc::file_exists };
            }

            if (auto res = co_await m_connection.rename(from, to, flags); not res) {
                co_return Unexpect{ res.error() };
            }
            if (m_cache) {
                co_await m_cache->rename(from_entry->id, to);
            }

            // parent must exist at this point
            auto to_parent = tree::Entry{
                .id   = to_entry->node.parent(),
                .node = m_tree.get(to_entry->node.parent())->get(),
            };

            co_return co_await move_node(*from_entry, to.filename(), to_parent);
        } break;
        case RenameMode::Exchange: {
            auto to_entry = co_await traverse_or_build_entry(to);
            if (not to_entry) {
                co_return Unexpect{ to_entry.error() };
            }
            auto err = m_tree.get(to_entry->id).transform([&](tree::Node& n) { return n.as_error(); });
            if (err and *err != nullptr) {
                co_return Unexpect{ (*err)->error };
            }

            if (auto res = co_await m_connection.rename(from, to, flags); not res) {
                co_return Unexpect{ res.error() };
            }
            if (m_cache) {
                co_await m_cache->rename(from_entry->id, to);
            }

            co_return co_await exchange_nodes(*from_entry, *to_entry);
        } break;
        case RenameMode::Normal: [[fallthrough]];
        default: {
            auto to_parent = co_await traverse_or_build_entry(to.parent_path());
            if (not to_parent) {
                co_return Unexpect{ to_parent.error() };
            }
            if (not to_parent->node.is_directory()) {
                co_return Unexpect{ Errc::not_a_directory };
            }

            if (auto res = co_await m_connection.rename(from, to, flags); not res) {
                co_return Unexpect{ res.error() };
            }
            if (m_cache) {
                co_await m_cache->rename(from_entry->id, to);
            }

            co_return co_await move_node(*from_entry, to.filename(), *to_parent);
        }
        }
    }

    AExpect<void> Filesystem::utimens(path::Path path, timespec atime, timespec mtime)
    {
        auto entry = co_await traverse_or_build_entry(path);
        if (not entry) {
            co_return Unexpect{ entry.error() };
        }
        if (auto res = co_await m_connection.utimens(path, atime, mtime); not res) {
            co_return Unexpect{ res.error() };
        }
        co_return (co_await m_connection.stat(path)).transform([&](Stat stat) {
            entry->node.set_stat(std::move(stat));
        });
    }

    AExpect<void> Filesystem::truncate(path::Path path, off_t size)
    {
        auto entry = co_await traverse_or_build_entry(path);
        auto file  = entry.and_then([](tree::Entry& e) { return e.node.as_regular(); });

        if (not file) {
            co_return Unexpect{ file.error() };
        }

        auto [id, node] = *entry;

        // flush first
        if (m_cache) {
            std::ignore = co_await m_cache->flush(entry->id);
        }

        if (auto res = co_await m_connection.truncate(path, size); not res) {
            co_return Unexpect{ res.error() };
        }

        auto old_size = static_cast<usize>(node.stat().size);
        auto new_size = static_cast<usize>(size);

        // error from Cache::truncate are from eviction only, which should not matter for this file
        if (m_cache) {
            std::ignore = co_await m_cache->truncate(id, old_size, new_size);
        }

        node.set_size(size);
        node.refresh_stat(timespec_omit, timespec_now);

        co_return Expect<void>{};
    }

    AExpect<u64> Filesystem::open(path::Path path, int flags)
    {
        auto entry = co_await traverse_or_build_entry(path);
        auto file  = entry.and_then([](tree::Entry& e) { return e.node.as_regular(); });

        if (not file) {
            co_return Unexpect{ file.error() };
        }

        auto [id, node] = *entry;

        auto mode = static_cast<OpenMode>(O_ACCMODE & flags);

        // send hint to cache to prepare a real fd that can be used for further operations
        if (m_cache) {
            co_return (co_await m_cache->hint_open(id, path, mode)).transform([&] {
                return m_handles.store(id, mode, 0);
            });
        } else {
            co_return (co_await m_connection.open(path, mode)).transform([&](u64 real_fd) {
                return m_handles.store(id, mode, real_fd);
            });
        }
    }

    AExpect<usize> Filesystem::read(u64 fd, Span<char> out, off_t offset)
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
            node->get().refresh_stat(timespec_now, timespec_omit);
            return ret;
        };

        if (m_cache) {
            co_return (co_await m_cache->read(handle->id, out, offset)).transform(after);
        } else {
            assert(handle->real_fd != 0 && "on no-cache, the file descriptor is exposed directly, not 0");
            co_return (co_await m_connection.read(handle->real_fd, out, offset)).transform(after);
        }
    }

    AExpect<usize> Filesystem::write(u64 fd, Str in, off_t offset)
    {
        auto handle = m_handles.find(fd, OpenMode::Write);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto may_file = node->get().as_regular();
        if (not may_file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& file = may_file->get();

        auto after = [&](usize ret) {
            // the file size is defined as offset + size from last write if it's higher than previous size
            auto new_size = offset + static_cast<off_t>(ret);
            auto size     = std::max(node->get().stat().size, new_size);

            node->get().set_size(size);
            node->get().refresh_stat(timespec_omit, timespec_now);

            file.dirty = true;

            return ret;
        };

        if (m_cache) {
            co_return (co_await m_cache->write(handle->id, in, offset)).transform(after);
        } else {
            co_return (co_await m_connection.write(handle->real_fd, in, offset)).transform(after);
        }
    }

    AExpect<void> Filesystem::flush(u64 fd)
    {
        auto handle = m_handles.find(fd);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto may_file = node->get().as_regular();
        if (not may_file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& file = may_file->get();
        if (not file.dirty) {
            co_return Expect<void>{};    // no writes, do nothing
        }

        if (m_cache) {
            co_return (co_await m_cache->flush(handle->id)).transform([&] {
                node->get().refresh_stat(timespec_omit, timespec_now);
                file.dirty = false;
            });
        } else {
            // do nothing, write already happens on write :P
            file.dirty = false;
            co_return Expect<void>{};
        }
    }

    AExpect<void> Filesystem::release(u64 fd)
    {
        auto handle = m_handles.release(fd);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto node = m_tree.get(handle->id);
        if (not node) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto may_file = node->get().as_regular();
        if (not may_file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& file = may_file->get();
        if (file.dirty and m_cache) {
            if (auto res = co_await m_cache->flush(handle->id); not res) {
                co_return Unexpect{ res.error() };
            }

            node->get().refresh_stat(timespec_omit, timespec_now);
            file.dirty = false;
        }

        // send hint to cache to close its associated fd for this node if exist
        if (m_cache) {
            co_return co_await m_cache->hint_close(handle->id, handle->mode);
        } else {
            co_return co_await m_connection.close(handle->real_fd);
        }
    }

    AExpect<usize> Filesystem::copy_file_range(
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
        std::ignore = co_await flush(in_fd);
        std::ignore = co_await flush(out_fd);

        auto in_entry = traverse_entry(in_path);
        if (not in_entry) {
            co_return Unexpect{ in_entry.error() };
        }

        auto out_entry = traverse_entry(out_path);
        if (not out_entry) {
            co_return Unexpect{ out_entry.error() };
        }

        auto copied = co_await m_connection.copy_file_range(in_path, in_off, out_path, out_off, size);
        if (not copied) {
            co_return Unexpect{ copied.error() };
        }

        co_return (co_await m_connection.stat(out_path)).and_then([&](Stat new_stat) {
            in_entry->node.refresh_stat(timespec_now, timespec_omit);
            out_entry->node.set_stat(new_stat);
            return copied;
        });
    }

    Expect<void> Filesystem::symlink(path::Path path, Str target)
    {
        auto parent  = traverse_entry(path.parent_path());
        auto may_dir = parent.and_then([](tree::Entry& e) { return e.node.as_directory(); });

        if (not may_dir) {
            return Unexpect{ may_dir.error() };
        }

        auto& dir  = may_dir->get();
        auto  name = path.filename();

        if (dir.find(name)) {
            return Unexpect{ Errc::file_exists };
        }

        // NOTE: we can't really make a symlink on android from adb (unless rooted device iirc), so this
        // operation actually not creating any link on the adb device, just on the in-memory filetree.

        auto now  = SystemClock::now().time_since_epoch();
        auto sec  = std::chrono::duration_cast<Seconds>(now);
        auto nsec = std::chrono::duration_cast<Nanoseconds>(now - sec);

        auto time = timespec{ .tv_sec = sec.count(), .tv_nsec = nsec.count() };

        // dummy stat for symlink based on
        // lrw-r--r--  root root 21 2024-10-05 09:19:29.000000000 +0700 /sdcard -> /storage/self/primary
        auto stat = Stat{
            .links = 1,
            .size  = 21,
            .mtime = time,
            .atime = time,
            .ctime = time,
            .mode  = S_IFLNK | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,    // mode: "lrw-r--r--"
            .uid   = 0,
            .gid   = 0,
        };

        auto link = tree::node::Link{ String{ target } };
        return m_tree.create_node(parent->id, name, std::move(stat), std::move(link)).transform(sink_void);
    }

    AExpect<void> Filesystem::initialize(path::Path path)
    {
        if (auto stat = co_await m_connection.stat(path::Path{}); stat.has_value()) {
            m_tree.root_node().set_stat(*stat);
            if (not path.is_root()) {
                std::ignore = co_await traverse_or_build(path);
            }
        } else {
            co_return Unexpect{ stat.error() };
        }

        if (m_cache) {
            co_await m_cache->initialize();
        }

        co_return Expect<void>{};
    }

    Await<void> Filesystem::shutdown()
    {
        if (m_cache) {
            co_await m_cache->shutdown();
        }
    }

    Opt<Seconds> Filesystem::set_ttl(Opt<Seconds> ttl)
    {
        auto old = std::exchange(m_ttl, ttl);
        if (old == ttl) {
            return old;
        }

        // on change from ttl off to ttl on, sets all nodes expiration to that new ttl
        // on change from ttl on to ttl off, sets all nodes expiration to never

        log_i(__func__, "ttl changed [{} -> {}] resetting expirations", old, ttl);
        walk(m_tree.root(), [=](tree::Entry e) { e.node.expires_after(ttl.value_or(Seconds::max())); });

        return old;
    }

    usize Filesystem::expires_all()
    {
        auto count = 0uz;
        walk(m_tree.root(), [&](tree::Entry e) { ++count, e.node.expires_after(Seconds{ 0 }); });
        return count;
    }
}
