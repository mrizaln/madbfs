#include "adbfsm/tree/node.hpp"
#include "adbfsm/data/connection.hpp"
#include "adbfsm/util/overload.hpp"

#include <fcntl.h>

namespace adbfsm::tree
{
    Expect<Ref<Node>> Directory::find(Str name) const
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; });
        if (found == m_children.end()) {
            return Unexpect{ Errc::no_such_file_or_directory };
        }
        return *found->get();
    }

    bool Directory::erase(Str name)
    {
        return std::erase_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; }) > 0;
    }

    Expect<Pair<Ref<Node>, Uniq<Node>>> Directory::insert(Uniq<Node> node, bool overwrite)
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == node->name(); });

        if (found == m_children.end()) {
            auto& back = m_children.emplace_back(std::move(node));
            return Pair{ std::ref(*back.get()), nullptr };
        } else if (overwrite) {
            auto node_ptr = node.get();
            auto released = std::exchange(*found, std::move(node));
            return Pair{ std::ref(*node_ptr), std::move(released) };
        }

        return Unexpect{ Errc::file_exists };
    }

    Expect<Uniq<Node>> Directory::extract(Str name)
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; });
        if (found == m_children.end()) {
            return Unexpect{ Errc::no_such_file_or_directory };
        }

        auto node = std::move(*found);
        m_children.erase(found);

        return node;
    }
}

namespace adbfsm::tree
{
    Expect<Ref<const data::Stat>> Node::stat() const
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return m_stat;
    }

    Str Node::printable_type() const
    {
        auto visitor = util::Overload{
            [](const RegularFile&) { return "file"; },       //
            [](const Directory&) { return "directory"; },    //
            [](const Link&) { return "link"; },              //
            [](const Other&) { return "other"; },            //
            [](const Error&) { return "error"; },            //
        };
        return std::visit(visitor, m_value);
    }

    path::PathBuf Node::build_path() const
    {
        auto path = m_name | sv::reverse | sr::to<String>();
        auto iter = std::back_inserter(path);

        for (auto current = m_parent; current != nullptr; current = current->m_parent) {
            *iter = '/';
            sr::copy(current->m_name | sv::reverse, iter);
        }

        // if the last path is root, we need to remove the last /
        if (path.size() > 2 and path[path.size() - 1] == '/' and path[path.size() - 2] == '/') {
            path.pop_back();
        }
        sr::reverse(path);

        return path::create_buf(std::move(path)).value();
    }

    bool Node::has_synced() const
    {
        // should have locked but bool is atomic anyway in x64
        auto visit = util::Overload{
            [](const Directory& dir) { return dir.has_readdir(); },
            [](const auto&) { return true; },
        };
        return std::visit(visit, m_value);
    }

    void Node::set_synced()
    {
        // should have locked but bool is atomic anyway in x64
        std::ignore = as<Directory>().transform(&Directory::set_readdir);
    }

    Expect<Ref<Node>> Node::traverse(Str name) const
    {
        auto lock = strt::shared_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then(proj(&Directory::find, name));
    }

    Expect<void> Node::list(std::move_only_function<void(Str)>&& fn) const
    {
        auto lock = strt::shared_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().transform([&](const Directory& dir) {
            for (const auto& node : dir.children()) {
                if (not node->is<Error>()) {
                    fn(node->name());
                }
            }
        });
    }

    Expect<Ref<Node>> Node::build(Str name, data::Stat stat, File file)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>()
            .and_then(proj(
                &Directory::insert,
                std::make_unique<Node>(name, this, std::move(stat), std::move(file)),
                false
            ))
            .transform([](auto&& pair) { return pair.first; });
    }

    Expect<Uniq<Node>> Node::extract(Str name)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then(proj(&Directory::extract, name));
    }

    Expect<Pair<Ref<Node>, Uniq<Node>>> Node::insert(Uniq<Node> node, bool overwrite)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then(proj(&Directory::insert, std::move(node), overwrite));
    }

    Expect<Ref<Node>> Node::link(Str name, Node* target)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            if (dir.find(name).has_value()) {
                return Unexpect{ Errc::file_exists };
            }

            // NOTE: we can't really make a symlink on android from adb (unless rooted device iirc), so this
            // operation actually not creating any link on the adb device, just on the in-memory filetree.

            // dummy stat for symlink based on
            // lrw-r--r--  root root 21 2024-10-05 09:19:29.000000000 +0700 /sdcard -> /storage/self/primary
            auto dummy_stat = data::Stat{
                .links = 1,
                .size  = 21,
                .mtime = Clock::to_time_t(Clock::now()),
                .mode  = S_IFLNK | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,    // mode: "lrw-r--r--"
                .uid   = 0,
                .gid   = 0,
            };

            auto node = std::make_unique<Node>(name, this, std::move(dummy_stat), Link{ target });
            return dir.insert(std::move(node), false).transform([&](auto&& pair) { return pair.first; });
        });
    }

    Expect<Ref<Node>> Node::touch(Context context, Str name)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            auto overwrite = false;
            if (auto node = dir.find(name); node.has_value()) {
                if (not node->get().is<Error>()) {
                    node.transform(&Node::refresh_stat).value();
                    return context.connection.touch(context.path, false).transform([&] { return *node; });
                }
                overwrite = true;
            }
            return context.connection.touch(context.path, true)
                .and_then([&] { return context.connection.stat(context.path); })
                .and_then([&](data::ParsedStat&& parsed) {
                    auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), RegularFile{});
                    return dir.insert(std::move(node), overwrite);
                })
                .transform([&](auto&& pair) { return pair.first; });
        });
    }

    Expect<Ref<Node>> Node::mkdir(Context context, Str name)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            auto overwrite = false;
            if (auto node = dir.find(name); node.has_value()) {
                if (not node->get().is<Error>()) {
                    return Unexpect{ Errc::file_exists };
                }
                overwrite = true;
            }
            return context.connection.mkdir(context.path)
                .and_then([&] { return context.connection.stat(context.path); })
                .and_then([&](data::ParsedStat&& parsed) {
                    auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), Directory{});
                    return dir.insert(std::move(node), overwrite);
                })
                .transform([&](auto&& pair) { return pair.first; });
        });
    }

    Expect<void> Node::rm(Context context, Str name, bool recursive)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then([&](Directory& dir) -> Expect<void> {
            return dir.find(name).and_then([&](Node& node) -> Expect<void> {
                if (node.is<Directory>() and not recursive) {
                    return Unexpect{ Errc::is_a_directory };
                }
                auto success = dir.erase(name);
                assert(success);
                return context.connection.rm(context.path, recursive);
            });
        });
    }

    Expect<void> Node::rmdir(Context context, Str name)
    {
        auto lock = strt::exclusive_lock{ m_mutex };
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then([&](Directory& dir) {
            return dir.find(name).and_then([&](Node& node) -> Expect<void> {
                return node.as<Directory>().and_then([&](Directory& target) {
                    return target.children().empty()
                             ? context.connection.rmdir(context.path).transform([&] { dir.erase(name); })
                             : Unexpect{ Errc::directory_not_empty };
                });
            });
        });
    }

    Expect<void> Node::truncate(Context context, off_t size)
    {
        auto lock = strt::shared_lock{ m_mutex };
        return regular_file_prelude().and_then([&]([[maybe_unused]] RegularFile& file) {
            return context.connection.truncate(context.path, size).transform([&] { m_stat.size = size; });
        });
    }

    Expect<u64> Node::open(Context context, int flags)
    {
        auto lock = strt::shared_lock{ m_mutex };
        return regular_file_prelude().and_then([&](RegularFile& file) {
            auto read_lock = strt::shared_lock{ m_mutex_file };
            return context.connection.open(context.path, flags).transform([&](u64 fd) {
                auto res = file.open(fd, flags);
                assert(res);
                return fd;
            });
        });
    }

    Expect<usize> Node::read(Context context, u64 fd, Span<char> out, off_t offset)
    {
        return regular_file_prelude().and_then([&](RegularFile& file) -> Expect<usize> {
            if (not file.is_open(fd)) {
                return Unexpect{ Errc::bad_file_descriptor };
            }
            auto read_lock = strt::shared_lock{ m_mutex_file };
            return context.cache.read(id(), out, offset, [&context](Span<char> out, off_t offset) {
                return context.connection.read(context.path, out, offset);
            });
        });
    }

    Expect<usize> Node::write(Context context, u64 fd, Str in, off_t offset)
    {
        return regular_file_prelude().and_then([&](RegularFile& file) -> Expect<usize> {
            if (not file.is_open(fd)) {
                return Unexpect{ Errc::bad_file_descriptor };
            }
            auto write_lock = strt::exclusive_lock{ m_mutex_file };
            file.set_dirty(true);
            return context.cache.write(id(), in, offset).transform([&](usize ret) {
                m_stat.size += ret;
                return ret;
            });
        });
    }

    Expect<void> Node::flush(Context context, u64 fd)
    {
        return regular_file_prelude().and_then([&](RegularFile& file) -> Expect<void> {
            if (not file.is_open(fd)) {
                return Unexpect{ Errc::bad_file_descriptor };
            }
            if (not file.is_dirty()) {
                return {};    // no write, do nothing
            }
            file.set_dirty(false);
            auto write_lock = strt::exclusive_lock{ m_mutex_file };
            auto filesize   = static_cast<usize>(stat()->get().size);
            return context.cache.flush(id(), filesize, [&](Span<const char> in, off_t offset) {
                return context.connection.write(context.path, in, offset);
            });
        });
    }

    Expect<void> Node::release(Context context, u64 fd)
    {
        return regular_file_prelude().and_then([&](RegularFile& file) -> Expect<void> {
            if (not file.close(fd)) {
                return Unexpect{ Errc::bad_file_descriptor };
            }
            if (not file.is_dirty()) {
                return {};    // no write, do nothing
            }
            file.set_dirty(false);
            auto write_lock = strt::exclusive_lock{ m_mutex_file };
            auto filesize   = static_cast<usize>(stat()->get().size);
            return context.cache.flush(id(), filesize, [&](Span<const char> in, off_t offset) {
                return context.connection.write(context.path, in, offset);
            });
        });
    }

    Expect<void> Node::utimens(Context context)
    {
        auto lock = strt::shared_lock{ m_mutex };
        return context.connection.touch(context.path, false)
            .and_then([&] { return context.connection.stat(context.path); })
            .transform([&](data::ParsedStat&& parsed) { m_stat = parsed.stat; });
    }

    Expect<Ref<Node>> Node::readlink()
    {
        auto lock    = strt::shared_lock{ m_mutex };
        auto current = this;
        while (current->is<Link>()) {
            current = &current->as<Link>()->get().target();
        }
        return *current;
    }
}
