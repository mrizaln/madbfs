#include "adbfsm/tree/node.hpp"
#include "adbfsm/data/connection.hpp"

#include <fcntl.h>
#include <mutex>

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
        auto lock = std::shared_lock{ m_mutex };
        return as<Directory>().and_then(proj(&Directory::find, name));
    }

    Expect<void> Node::list(std::move_only_function<void(Str)>&& fn) const
    {
        auto lock = std::shared_lock{ m_mutex };
        return as<Directory>().transform([&](const Directory& dir) {
            for (const auto& node : dir.children()) {
                fn(node->name());
            }
        });
    }

    Expect<Ref<Node>> Node::build(Str name, data::Stat stat, File file)
    {
        auto lock = std::unique_lock{ m_mutex };
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
        auto lock = std::unique_lock{ m_mutex };
        return as<Directory>().and_then(proj(&Directory::extract, name));
    }

    Expect<Pair<Ref<Node>, Uniq<Node>>> Node::insert(Uniq<Node> node, bool overwrite)
    {
        auto lock = std::unique_lock{ m_mutex };
        return as<Directory>().and_then(proj(&Directory::insert, std::move(node), overwrite));
    }

    Expect<Ref<Node>> Node::link(Str name, Node* target)
    {
        auto lock = std::unique_lock{ m_mutex };

        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            if (dir.find(name).has_value()) {
                return Unexpect{ Errc::file_exists };
            }

            // NOTE: we can't really make a symlink on android from adb (unless rooted device iirc), so this
            // operation actuall not creating any link on the adb device, just on the in-memory filetree.

            // dummy stat for symlink based on
            // lrw-r--r--  root root 21 2024-10-05 09:19:29.000000000 +0700 /sdcard -> /storage/self/primary
            auto dummy_stat = data::Stat{
                .mode  = S_IFLNK | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,    // mode: "lrw-r--r--"
                .links = 1,
                .uid   = 0,
                .gid   = 0,
                .size  = 21,
                .mtime = Clock::to_time_t(Clock::now()),
            };

            auto node = std::make_unique<Node>(name, this, std::move(dummy_stat), Link{ target });
            return dir.insert(std::move(node), false).transform([&](auto&& pair) { return pair.first; });
        });
    }

    Expect<Ref<Node>> Node::touch(Context context, Str name)
    {
        auto lock = std::unique_lock{ m_mutex };

        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            if (auto node = dir.find(name); node.has_value()) {
                node.transform(proj(&Node::refresh_stat)).value();
                return context.connection.touch(context.path, false).transform([&] { return *node; });
            }
            return context.connection.touch(context.path, true)
                .and_then([&] { return context.connection.stat(context.path); })
                .and_then([&](data::ParsedStat&& parsed) {
                    auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), RegularFile{});
                    return dir.insert(std::move(node), false);
                })
                .transform([&](auto&& pair) { return pair.first; });
        });
    }

    Expect<Ref<Node>> Node::mkdir(Context context, Str name)
    {
        auto lock = std::unique_lock{ m_mutex };

        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            if (dir.find(name).has_value()) {
                return Unexpect{ Errc::file_exists };
            }
            return context.connection.mkdir(context.path)
                .and_then([&] { return context.connection.stat(context.path); })
                .and_then([&](data::ParsedStat&& parsed) {
                    auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), Directory{});
                    return dir.insert(std::move(node), false);
                })
                .transform([&](auto&& pair) { return pair.first; });
        });
    }

    Expect<void> Node::rm(Context context, Str name, bool recursive)
    {
        auto lock = std::unique_lock{ m_mutex };

        return as<Directory>().and_then([&](Directory& dir) -> Expect<void> {
            return dir.find(name).and_then([&](Node& node) -> Expect<void> {
                if (node.is<Directory>() and not recursive) {
                    return Unexpect{ Errc::is_a_directory };
                }

                if (node.is<RegularFile>()) {
                    auto res = node.as<RegularFile>().and_then([&](RegularFile& file) {
                        return context.cache.remove(context.connection, file.id());
                    });
                    if (not res.has_value()) {
                        return Unexpect{ res.error() };
                    }
                }

                auto success = dir.erase(name);
                assert(success);
                return context.connection.rm(context.path, recursive);
            });
        });
    }

    Expect<void> Node::rmdir(Context context, Str name)
    {
        auto lock = std::unique_lock{ m_mutex };

        return as<Directory>().and_then([&](Directory& dir) {
            return dir.find(name).and_then([&](Node& node) {
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
        // clang-format off
        if      (is<Directory>()) return Unexpect{ Errc::is_a_directory };
        else if (is<Other>())     return Unexpect{ Errc::permission_denied };
        else if (is<Link>())      return as<Link>()->get().target().truncate(context, size);
        // clang-format on

        auto  lock = std::unique_lock{ m_mutex };
        auto& file = as<RegularFile>()->get();

        auto path = context.cache.get(file.id());
        if (not path.has_value()) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return Unexpect{ id.error() };
            }
            path = context.cache.get(*id);
            file.set_id(*id);
        }

        context.cache.set_dirty(file.id(), true);
        auto ret = ::truncate(path->as_path().fullpath().data(), size);
        if (ret < 0) {
            return Unexpect{ static_cast<Errc>(errno) };
        }

        m_stat.size -= size;
        return {};
    }

    Expect<i32> Node::open(Context context, int flags)
    {
        // clang-format off
        if      (is<Directory>()) return Unexpect{ Errc::is_a_directory };
        else if (is<Other>())     return Unexpect{ Errc::permission_denied };
        else if (is<Link>())      return as<Link>()->get().target().open(context, flags);
        // clang-format on

        auto  lock = std::unique_lock{ m_mutex };
        auto& file = as<RegularFile>()->get();

        auto path = context.cache.get(file.id());
        if (not path.has_value()) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return Unexpect{ id.error() };
            }
            path = context.cache.get(*id);
            file.set_id(*id);
        }

        auto ret = ::open(path->as_path().fullpath().data(), flags);
        if (ret >= 0) {
            file.set_fd(ret);
        }

        return Unexpect{ ret < 0 ? static_cast<Errc>(errno) : Errc{} };
    }

    Expect<usize> Node::read(Context context, std::span<char> out, off_t offset)
    {
        // clang-format off
        if      (is<Directory>()) return Unexpect{ Errc::is_a_directory };
        else if (is<Other>())     return Unexpect{ Errc::permission_denied };
        else if (is<Link>())      return as<Link>()->get().target().read(context, out, offset);
        // clang-format on

        auto  lock = std::unique_lock{ m_mutex };
        auto& file = as<RegularFile>()->get();

        // in-case the cache is missing, add it back
        if (not context.cache.exists(file.id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return Unexpect{ id.error() };
            }
            file.set_id(*id);
        }

        if (auto ret = ::pread(file.fd(), out.data(), out.size(), offset); ret >= 0) {
            return ret;
        }
        return Unexpect{ static_cast<Errc>(errno) };
    }

    Expect<usize> Node::write(Context context, std::string_view in, off_t offset)
    {
        // clang-format off
        if      (is<Directory>()) return Unexpect{ Errc::is_a_directory };
        else if (is<Other>())     return Unexpect{ Errc::permission_denied };
        else if (is<Link>())      return as<Link>()->get().target().write(context, in, offset);
        // clang-format on

        auto  lock = std::unique_lock{ m_mutex };
        auto& file = as<RegularFile>()->get();

        // in-case the cache is missing, add it back and set to dirty
        if (not context.cache.exists(file.id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return Unexpect{ id.error() };
            }
            file.set_id(*id);
        }

        if (auto ret = ::pwrite(file.fd(), (void*)in.data(), in.size(), offset); ret >= 0) {
            context.cache.set_dirty(file.id(), true);
            m_stat.size += ret;
            return ret;
        }
        return Unexpect{ static_cast<Errc>(errno) };
    }

    Expect<void> Node::flush(Context context)
    {
        // clang-format off
        if      (is<Directory>()) return Unexpect{ Errc::is_a_directory };
        else if (is<Other>())     return Unexpect{ Errc::permission_denied };
        else if (is<Link>())      return as<Link>()->get().target().flush(context);
        // clang-format on

        auto  lock = std::unique_lock{ m_mutex };
        auto& file = as<RegularFile>()->get();

        // in-case the cache is missing, add it back
        if (not context.cache.exists(file.id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return Unexpect{ id.error() };
            }
            file.set_id(*id);
        }

        auto ret = ::fsync(file.fd());
        return Unexpect{ ret < 0 ? static_cast<Errc>(errno) : Errc{} };
    }

    Expect<void> Node::release(Context context)
    {
        // clang-format off
        if      (is<Directory>()) return Unexpect{ Errc::is_a_directory };
        else if (is<Other>())     return Unexpect{ Errc::permission_denied };
        else if (is<Link>())      return as<Link>()->get().target().flush(context);
        // clang-format on

        auto  lock = std::unique_lock{ m_mutex };
        auto& file = as<RegularFile>()->get();

        // in-case the cache is missing
        // TODO: should have assert that file exist
        if (not context.cache.exists(file.id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return Unexpect{ id.error() };
            }
            file.set_id(*id);
        }

        auto ret = ::close(file.fd());
        if (auto res = context.cache.flush(context.connection, file.id()); not res.has_value()) {
            return Unexpect{ res.error() };
        }
        context.cache.set_dirty(file.id(), false);

        return Unexpect{ ret < 0 ? static_cast<Errc>(errno) : Errc{} };
    }

    Expect<void> Node::utimens(Context context)
    {
        auto lock = std::shared_lock{ m_mutex };
        return context.connection.touch(context.path, false)
            .and_then([&] { return context.connection.stat(context.path); })
            .transform([&](data::ParsedStat&& parsed) { m_stat = parsed.stat; });
    }

    Expect<Ref<Node>> Node::readlink()
    {
        auto lock    = std::shared_lock{ m_mutex };
        auto current = this;
        while (current->is<Link>()) {
            current = &current->as<Link>()->get().target();
        }
        return *current;
    }
}
