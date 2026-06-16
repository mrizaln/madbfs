#include "madbfs/filesystem.hpp"

#include "madbfs/connection.hpp"
#include "madbfs/node.hpp"

#include <madbfs-common/log.hpp>

#include <fmt/std.h>
#include <sys/stat.h>

#include <cassert>

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
        , m_root{ "/", nullptr, {}, node::Directory{} }
        , m_cache{ construct_cache(ctx, connection, caching) }
        , m_ttl{ ttl }
    {
    }

    AExpect<Ref<Node>> Filesystem::build(Node& parent, path::Path path)
    {
        const auto name = path.filename();

        auto build_then_expire = [&](Str name, Stat stat, File file) {
            return parent.build(name, std::move(stat), std::move(file)).transform([&](Node& node) {
                node.expires_after(m_ttl.value_or(Seconds::max()));
                return std::ref(node);
            });
        };

        auto stat = co_await m_connection.stat(path);
        if (not stat.has_value()) {
            auto err = stat.error();
            if (should_cache_error(err)) {
                std::ignore = build_then_expire(name, {}, node::Error{ err });
            }
            co_return Unexpect{ err };
        }

        switch (stat->mode & S_IFMT) {
        case S_IFREG: co_return build_then_expire(name, *stat, node::Regular{});
        case S_IFDIR: co_return build_then_expire(name, *stat, node::Directory{});
        case S_IFLNK: co_return build_then_expire(name, *stat, node::Link{});
        default: co_return build_then_expire(name, *stat, node::Other{});
        }
    }

    AExpect<Ref<Node>> Filesystem::build_directory(Node& parent, path::Path path)
    {
        const auto name = path.filename();

        auto build_then_expire = [&](Str name, Stat stat, File file) {
            return parent.build(name, std::move(stat), std::move(file)).transform([&](Node& node) {
                node.expires_after(m_ttl.value_or(Seconds::max()));
                return std::ref(node);
            });
        };

        auto stat = co_await m_connection.stat(path);
        if (not stat.has_value()) {
            auto err = stat.error();
            if (should_cache_error(err)) {
                std::ignore = build_then_expire(name, {}, node::Error{ err });
            }
            co_return Unexpect{ err };
        } else if ((stat->mode & S_IFMT) != S_IFDIR) {
            co_return Unexpect{ Errc::not_a_directory };
        }

        co_return build_then_expire(name, *stat, node::Directory{});
    }

    Expect<Ref<Node>> Filesystem::traverse(path::Path path)
    {
        if (path.is_root()) {
            return m_root;
        }

        auto* current = &m_root;

        for (auto name : path.iter()) {
            auto next = current->traverse(name);
            if (not next.has_value()) {
                return Unexpect{ next.error() };
            }
            current = &next->get();
        }

        return *current;
    }

    AExpect<Ref<Node>> Filesystem::traverse_or_build(path::Path path)
    {
        if (path.is_root()) {
            co_return m_root;
        }

        auto* current      = &m_root;
        auto  current_path = path::PathBuf{};

        // iterate until parent
        for (auto name : path.parent_path().iter()) {
            current_path.extend(name);
            if (auto next = current->traverse(name); next.has_value()) {
                if (auto& node = next->get(); node.expired()) {
                    if (auto res = co_await update(node, current_path); not res) {
                        co_return Unexpect{ res.error() };
                    }
                }
                current = &next->get();
                continue;
            }

            auto next = co_await build_directory(*current, current_path);
            if (not next) {
                co_return Unexpect{ next.error() };
            }

            current = &next->get();
        }

        current_path.extend(path.filename());
        if (auto found = current->traverse(path.filename()); found.has_value()) {
            if (auto& node = found->get(); node.expired()) {
                if (auto res = co_await update(node, current_path); not res) {
                    co_return Unexpect{ res.error() };
                }
            }
            co_return found;
        }

        co_return co_await build(*current, current_path);
    }

    AExpect<void> Filesystem::update(Node& node, path::Path path)
    {
        log_d(__func__, "{:?}", path);

        auto new_stat = co_await m_connection.stat(path);
        auto old_stat = node.stat();

        if (not new_stat) {
            auto err = new_stat.error();
            if (should_cache_error(err)) {
                co_await mutate_and_invalidate(node, node::Error{ err });
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
            co_await mutate_and_invalidate(node, node::Regular{});    // invalidate currently held data
            node.expires_after(m_ttl.value_or(Seconds::max()));
        } break;
        case S_IFDIR: {
            if (S_ISDIR(old_stat.mode)) {    // previously directory
                node.set_stat(*new_stat);
                node.set_synced(false);    // don't mutate, force rescan
                node.expires_after(m_ttl.value_or(Seconds::max()));
            } else {
                node.set_stat(*new_stat);
                co_await mutate_and_invalidate(node, node::Directory{});    // not directory, becomes one
                node.expires_after(m_ttl.value_or(Seconds::max()));
            }
        } break;
        case S_IFLNK: {
            node.set_stat(*new_stat);
            co_await mutate_and_invalidate(node, node::Link{});
            node.expires_after(m_ttl.value_or(Seconds::max()));
        } break;
        default: {
            node.set_stat(*new_stat);
            co_await mutate_and_invalidate(node, node::Other{});
            node.expires_after(m_ttl.value_or(Seconds::max()));
        } break;
        }

        co_return Expect<void>{};
    }

    Await<void> Filesystem::mutate_and_invalidate(Node& node, File file)
    {
        auto old = node.mutate(std::move(file));

        if (auto dir = std::get_if<node::Directory>(&old)) {
            auto nodes = std::vector<Node*>{};
            for (const auto& node : dir->children()) {
                walk(*node, [&](Node& n) { m_handles.erase(&n), nodes.push_back(&n); });
            }
            if (m_cache) {
                for (auto node : nodes) {
                    co_await m_cache->invalidate_one(node->id(), false);    // flush? child maybe unchanged
                }
            }
            m_handles.erase(&node);
        } else if (std::get_if<node::Regular>(&old)) {
            if (m_cache) {
                co_await m_cache->invalidate_one(node.id(), false);    // no flush since the file changed
            }
            m_handles.erase(&node);
        }
    }

    void Filesystem::walk(Node& start, std::function<void(Node&)> func)
    {
        auto stack = Vec<Node*>{ &start };

        while (not stack.empty()) {
            auto node = stack.back();
            stack.pop_back();

            func(*node);

            if (auto dir = node->as_directory(); dir) {
                for (const auto& node : dir->get().children()) {
                    stack.push_back(node.get());
                }
            }
        }
    }

    AExpect<void> Filesystem::readdir(path::Path path, Filler filler)
    {
        auto current = &m_root;

        if (not path.is_root()) {
            auto maybe_node = co_await traverse_or_build(path);
            if (not maybe_node.has_value()) {
                co_return Unexpect{ maybe_node.error() };
            }
            current = &maybe_node->get();
        }

        auto current_dir = current->as_directory();
        if (not current_dir) {
            co_return Unexpect{ current_dir.error() };
        }

        auto& list    = current_dir->get().children();
        auto  pathbuf = path.extend_copy("dummy").value();

        auto build_file = [&](Str name, mode_t mode) -> Await<File> {
            auto renamed = pathbuf.rename(name);
            assert(renamed);

            switch (mode & S_IFMT) {
            case S_IFREG: co_return node::Regular{};
            case S_IFDIR: co_return node::Directory{};
            case S_IFLNK: co_return node::Link{};
            default: co_return node::Other{};
            }
        };

        if (not current->has_synced()) {
            auto may_stats = co_await m_connection.statdir(path);
            if (not may_stats) {
                co_return Unexpect{ may_stats.error() };
            }

            if (list.empty()) {
                for (auto [stat, name] : may_stats.value()) {
                    log_d(__func__, "[{:?}] new entry    : {:?}", current->name(), name);

                    auto file  = co_await build_file(name, stat.mode);
                    auto child = std::make_unique<Node>(name, current, std::move(stat), std::move(file));
                    child->expires_after(m_ttl.value_or(Seconds::max()));
                    list.emplace(std::move(child));
                }
            } else {
                auto new_list = std::unordered_set<Str>{};

                // update old entries or add a new one if not exists
                for (auto [stat, name] : may_stats.value()) {
                    new_list.emplace(name);

                    auto found = list.find(name);
                    if (found == list.end()) {
                        log_d(__func__, "[{:?}] new entry: {:?}", current->name(), name);

                        auto file  = co_await build_file(name, stat.mode);
                        auto child = std::make_unique<Node>(name, current, std::move(stat), std::move(file));
                        child->expires_after(m_ttl.value_or(Seconds::max()));
                        list.emplace(std::move(child));

                        continue;
                    }

                    auto& child = (**found);
                    if (child.is_error()) {    // Error node
                        log_d(__func__, "[{:?}]   changed: {:?}", current->name(), name);

                        auto file = co_await build_file(name, stat.mode);
                        child.set_stat(std::move(stat));
                        co_await mutate_and_invalidate(child, std::move(file));
                        child.expires_after(m_ttl.value_or(Seconds::max()));
                    } else if (child.expired() and detect_modification(child.stat(), stat)) {
                        log_d(__func__, "[{:?}]   changed: {:?}", current->name(), name);

                        auto file = co_await build_file(name, stat.mode);
                        child.set_stat(std::move(stat));
                        co_await mutate_and_invalidate(child, std::move(file));
                        child.expires_after(m_ttl.value_or(Seconds::max()));
                    }

                    log_d(__func__, "[{:?}] unchanged: {:?}", current->name(), name);
                }

                // remove old entries if doesn't exist in new entries
                for (auto it = list.begin(); it != list.end();) {
                    auto name = (**it).name();
                    if (not new_list.contains(name)) {
                        log_d(__func__, "[{:?}]   removed: {:?}", current->name(), name);
                        if (m_cache) {
                            co_await m_cache->invalidate_one((**it).id(), false);    // should I flush
                        }
                        it = list.erase(it);
                        continue;
                    }
                    ++it;
                }
            }

            current->set_synced(true);
        }

        for (const auto& node : std::as_const(list)) {
            if (not node->is_error()) {
                filler(node->name().data());
            }
        }

        co_return Expect<void>{};
    }

    AExpect<NamedStat> Filesystem::getattr(path::Path path)
    {
        co_return (co_await traverse_or_build(path)).and_then([](Node& node) -> Expect<NamedStat> {
            if (auto err = node.as_error(); err) {
                return Unexpect{ err->error };
            }
            return NamedStat{ .id = node.id(), .stat = node.stat() };
        });
    }

    AExpect<Str> Filesystem::readlink(path::Path path)
    {
        auto link = (co_await traverse_or_build(path)).and_then([](Node& node) { return node.as_link(); });

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

    AExpect<Ref<Node>> Filesystem::mknod(path::Path path, mode_t mode, dev_t dev)
    {
        auto parent  = co_await traverse_or_build(path.parent_path());
        auto may_dir = parent.and_then([](Node& node) { return node.as_directory(); });

        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir       = may_dir->get();
        auto  name      = path.filename();
        auto  overwrite = false;

        if (auto node = dir.find(name); node) {
            if (not node->get().is_error()) {
                co_return Unexpect{ Errc::file_exists };
            }
            overwrite = true;
        }

        if (auto created = co_await m_connection.mknod(path, mode, dev); not created) {
            parent->get().refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ created.error() };
        }

        co_return (co_await m_connection.stat(path))
            .and_then([&](Stat stat) {
                auto node = std::make_unique<Node>(name, &parent->get(), std::move(stat), node::Regular{});
                return dir.insert(std::move(node), overwrite);
            })
            .transform([&](auto&& pair) { return pair.first; });
    }

    AExpect<Ref<Node>> Filesystem::mkdir(path::Path path, mode_t mode)
    {
        auto parent  = co_await traverse_or_build(path.parent_path());
        auto may_dir = parent.and_then([](Node& node) { return node.as_directory(); });

        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir       = may_dir->get();
        auto  name      = path.filename();
        auto  overwrite = false;

        if (auto node = dir.find(name); node) {
            if (not node->get().is_error()) {
                co_return Unexpect{ Errc::file_exists };
            }
            overwrite = true;
        }

        if (auto created = co_await m_connection.mkdir(path, mode); not created) {
            parent->get().refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ created.error() };
        }

        co_return (co_await m_connection.stat(path))
            .and_then([&](Stat stat) {
                auto node = std::make_unique<Node>(name, &parent->get(), std::move(stat), node::Directory{});
                return dir.insert(std::move(node), overwrite);
            })
            .transform([&](auto&& pair) { return pair.first; });
    }

    AExpect<void> Filesystem::unlink(path::Path path)
    {
        auto parent  = co_await traverse_or_build(path.parent_path());    // what if path is not traversed yet
        auto may_dir = parent.and_then([](Node& node) { return node.as_directory(); });

        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir  = may_dir->get();
        auto  name = path.filename();

        auto erased = dir.find(name).and_then([&](Node& node) -> Expect<Uniq<Node>> {
            if (node.is_directory()) {
                return Unexpect{ Errc::is_a_directory };
            }
            auto erased = dir.erase(name);
            assert(erased.has_value());
            return std::move(erased).value();
        });

        if (not erased) {
            co_return Unexpect{ erased.error() };
        }

        if (auto res = co_await m_connection.unlink(path); not res) {
            auto overwritten = dir.insert(std::move(*erased), true);    // re-insert on failure :P
            assert(not overwritten.has_value());
            parent->get().refresh_stat(timespec_omit, timespec_now);
            co_return Unexpect{ res.error() };
        }

        m_handles.erase(erased->get());
        if (m_cache) {
            co_await m_cache->invalidate_one((*erased)->id(), false);
        }

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::rmdir(path::Path path)
    {
        auto parent  = co_await traverse_or_build(path.parent_path());    // what if path is not traversed yet
        auto may_dir = parent.and_then([](Node& node) { return node.as_directory(); });

        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir  = may_dir->get();
        auto  name = path.filename();

        auto target = dir.find(name).and_then([](Node& target) { return target.as_directory(); });
        if (not target) {
            co_return Unexpect{ target.error() };
        }

        // disallow erasing if all the children is not Error
        if (const auto& children = target->get().children(); not children.empty()) {
            if (not sr::all_of(children, [](const Uniq<Node>& node) { return node->is_error(); })) {
                co_return Unexpect{ Errc::directory_not_empty };
            }
        }

        co_return (co_await m_connection.rmdir(path)).transform([&] {
            parent->get().refresh_stat(timespec_omit, timespec_now);
            dir.erase(name);
        });
    }

    AExpect<void> Filesystem::rename(path::Path from, path::Path to, u32 flags)
    {
        // I don't think root can be moved, :P
        if (from.is_root()) {
            co_return Unexpect{ Errc::operation_not_supported };
        }

        auto from_node = co_await traverse_or_build(from);
        if (not from_node.has_value()) {
            co_return Unexpect{ from_node.error() };
        }

        auto  from_parent = from_node->get().parent();
        auto& from_dir    = from_parent->as_directory()->get();    // guaranteed to be directory

        auto to_parent = co_await traverse_or_build(to.parent_path());
        auto to_dir    = to_parent.and_then([](Node& node) { return node.as_directory(); });
        if (not to_dir) {
            co_return Unexpect{ to_dir.error() };
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
            if (to_node.has_value() and not to_node->get().is_error()) {
                co_return Unexpect{ Errc::file_exists };
            }
        }

        auto res = co_await m_connection.rename(from, to, flags);
        if (not res) {
            co_return Unexpect{ res.error() };
        }

        from_parent->refresh_stat(timespec_omit, timespec_now);
        to_parent->get().refresh_stat(timespec_omit, timespec_now);

        auto node = from_dir.extract(from.filename()).value();
        if (m_cache) {
            co_await m_cache->rename(node->id(), to);
        }

        node->set_name(to.filename());
        node->set_parent(&to_parent->get());
        auto overwritten = to_dir->get().insert(std::move(node), true).value();

        if ((flags & RENAME_EXCHANGE) != 0) {
            assert(overwritten.second != nullptr);
            auto node = std::move(overwritten).second;

            if (m_cache) {
                co_await m_cache->rename(node->id(), from);
            }
            node->set_name(from.filename());
            node->set_parent(from_parent);

            auto res = from_dir.insert(std::move(node), false);
            assert(res->second == nullptr);    // has extracted before
        } else if (overwritten.second != nullptr) {
            if (m_cache) {
                co_await m_cache->invalidate_one(overwritten.second->id(), false);
            }
            m_handles.erase(overwritten.second.get());
        }

        co_return Expect<void>{};
    }

    AExpect<void> Filesystem::utimens(path::Path path, timespec atime, timespec mtime)
    {
        auto node = co_await traverse_or_build(path);
        if (not node) {
            co_return Unexpect{ node.error() };
        }
        if (auto res = co_await m_connection.utimens(path, atime, mtime); not res) {
            co_return Unexpect{ res.error() };
        }
        co_return (co_await m_connection.stat(path)).transform([&](Stat stat) {
            node->get().set_stat(std::move(stat));
        });
    }

    AExpect<void> Filesystem::truncate(path::Path path, off_t size)
    {
        auto may_node = co_await traverse_or_build(path);
        auto may_file = may_node.and_then([](Node& node) { return node.as_regular(); });

        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }

        if (auto res = co_await m_connection.truncate(path, size); not res) {
            co_return Unexpect{ res.error() };
        }

        auto& node = may_node->get();

        auto old_size = static_cast<usize>(node.stat().size);
        auto new_size = static_cast<usize>(size);

        // error from Cache::truncate are from eviction only, which should not matter for this file
        if (m_cache) {
            std::ignore = co_await m_cache->truncate(node.id(), old_size, new_size);
        }

        node.set_size(size);
        node.refresh_stat(timespec_omit, timespec_now);

        co_return Expect<void>{};
    }

    AExpect<u64> Filesystem::open(path::Path path, int flags)
    {
        auto may_node = co_await traverse_or_build(path);
        auto may_file = may_node.and_then([](Node& node) { return node.as_regular(); });

        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }

        auto& node = may_node->get();
        auto  mode = static_cast<OpenMode>(O_ACCMODE & flags);

        // send hint to cache to prepare a real fd that can be used for further operations
        if (m_cache) {
            co_return (co_await m_cache->hint_open(node.id(), path, mode)).transform([&] {
                return m_handles.store(&node, mode, 0);
            });
        } else {
            co_return (co_await m_connection.open(path, mode)).transform([&](u64 real_fd) {
                return m_handles.store(&node, mode, real_fd);
            });
        }
    }

    AExpect<usize> Filesystem::read(u64 fd, Span<char> out, off_t offset)
    {
        auto handle = m_handles.find(fd, OpenMode::Read);
        if (not handle) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto after = [&](usize ret) {
            handle->node->refresh_stat(timespec_now, timespec_omit);
            return ret;
        };

        if (m_cache) {
            co_return (co_await m_cache->read(handle->node->id(), out, offset)).transform(after);
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

        auto may_file = handle->node->as_regular();
        if (not may_file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& file = may_file->get();

        auto after = [&](usize ret) {
            // the file size is defined as offset + size from last write if it's higher than previous size
            auto new_size = offset + static_cast<off_t>(ret);
            auto size     = std::max(handle->node->stat().size, new_size);

            handle->node->set_size(size);
            handle->node->refresh_stat(timespec_omit, timespec_now);

            file.dirty = true;

            return ret;
        };

        if (m_cache) {
            co_return (co_await m_cache->write(handle->node->id(), in, offset)).transform(after);
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

        auto may_file = handle->node->as_regular();
        if (not may_file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& file = may_file->get();
        if (not file.dirty) {
            co_return Expect<void>{};    // no writes, do nothing
        }

        if (m_cache) {
            co_return (co_await m_cache->flush(handle->node->id())).transform([&] {
                handle->node->refresh_stat(timespec_omit, timespec_now);
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

        auto may_file = handle->node->as_regular();
        if (not may_file) [[unlikely]] {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& file = may_file->get();
        if (file.dirty and m_cache) {
            if (auto res = co_await m_cache->flush(handle->node->id()); not res) {
                co_return Unexpect{ res.error() };
            }

            handle->node->refresh_stat(timespec_omit, timespec_now);
            file.dirty = false;
        }

        // send hint to cache to close its associated fd for this node if exist
        if (m_cache) {
            co_return co_await m_cache->hint_close(handle->node->id(), handle->mode);
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

        auto in_node = traverse(in_path);
        if (not in_node) {
            co_return Unexpect{ in_node.error() };
        }

        auto out_node = traverse(out_path);
        if (not out_node) {
            co_return Unexpect{ out_node.error() };
        }

        auto copied = co_await m_connection.copy_file_range(in_path, in_off, out_path, out_off, size);
        if (not copied) {
            co_return Unexpect{ copied.error() };
        }

        co_return (co_await m_connection.stat(out_path)).and_then([&](Stat new_stat) {
            in_node->get().refresh_stat(timespec_now, timespec_omit);
            out_node->get().set_stat(new_stat);
            return copied;
        });
    }

    Expect<void> Filesystem::symlink(path::Path path, Str target)
    {
        auto parent  = traverse(path.parent_path());
        auto may_dir = parent.and_then([](Node& node) { return node.as_directory(); });

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

        auto link = node::Link{ String{ target } };
        auto node = std::make_unique<Node>(name, &parent->get(), std::move(stat), std::move(link));

        return dir.insert(std::move(node), false).transform(sink_void);
    }

    AExpect<void> Filesystem::initialize()
    {
        if (not std::exchange(m_root_initialized, true)) {
            if (auto stat = co_await m_connection.stat(path::Path{}); stat.has_value()) {
                m_root.set_stat(*stat);
            } else {
                co_return Unexpect{ stat.error() };
            }
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
        walk(m_root, [=](Node& node) { node.expires_after(ttl.value_or(Seconds::max())); });

        return old;
    }

    usize Filesystem::expires_all()
    {
        auto count = 0uz;
        walk(m_root, [&](Node& node) { ++count, node.expires_after(Seconds{ 0 }); });
        return count;
    }
}
