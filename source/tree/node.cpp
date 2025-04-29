#include "adbfsm/tree/node.hpp"
#include "adbfsm/data/connection.hpp"

#include <fcntl.h>

namespace adbfsm::tree
{
    Expect<Node*> Directory::find(Str name) const
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; });
        if (found == m_children.end()) {
            return std::unexpected{ std::errc::no_such_file_or_directory };
        }
        return found->get();
    }

    bool Directory::erase(Str name)
    {
        return std::erase_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; }) > 0;
    }

    Expect<Opt<Uniq<Node>>> Directory::insert(Uniq<Node> node, bool overwrite)
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == node->name(); });

        if (found == m_children.end()) {
            m_children.push_back(std::move(node));
            return std::nullopt;
        } else if (overwrite) {
            auto released = std::exchange(*found, std::move(node));
            return released;
        }

        return std::unexpected{ std::errc::file_exists };
    }

    Expect<Uniq<Node>> Directory::extract(Str name)
    {
        auto found = sr::find_if(m_children, [&](const Uniq<Node>& n) { return n->name() == name; });
        if (found == m_children.end()) {
            return std::unexpected{ std::errc::no_such_file_or_directory };
        }

        auto node = std::move(*found);
        m_children.erase(found);

        return node;
    }
}

namespace adbfsm::tree
{
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
}

namespace adbfsm::tree
{
    Expect<Node*> Node::build(Str name, data::Stat stat, File file)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        auto node = std::make_unique<Node>(name, this, std::move(stat), std::move(file));
        return dir->insert(std::move(node), false).and_then([&](auto&& overwritten) {
            assert(not overwritten);    // NOTE: overwriting the value is not allowed on build mode,
            return dir->find(name);
        });
    }

    Expect<Uniq<Node>> Node::extract(Str name)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }
        return dir->extract(name);
    }

    Expect<Opt<Uniq<Node>>> Node::insert(Uniq<Node> node, bool overwrite)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }
        return dir->insert(std::move(node), overwrite);
    }

    Expect<Node*> Node::touch(Str name, Context context)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        return context.connection.touch(context.path, true)
            .and_then([&] { return context.connection.stat(context.path); })
            .and_then([&](data::ParsedStat&& parsed) {
                auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), RegularFile{});
                return dir->insert(std::move(node), false);
            })
            .and_then([&](auto&& overwrite) {
                assert(not overwrite);
                return dir->find(name);
            });
    }

    Expect<Node*> Node::mkdir(Str name, Context context)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (dir->find(name).has_value()) {
            return std::unexpected{ std::errc::file_exists };
        }

        return context.connection.mkdir(context.path)
            .and_then([&] { return context.connection.stat(context.path); })
            .and_then([&](data::ParsedStat&& parsed) {
                auto node = std::make_unique<Node>(name, this, std::move(parsed.stat), Directory{});
                return dir->insert(std::move(node), false);
            })
            .and_then([&](auto&& overwrite) {
                assert(not overwrite);
                return dir->find(name);
            });
    }

    Expect<Node*> Node::link(Str name, Node* target, [[maybe_unused]] Context context)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        if (dir->find(name).has_value()) {
            return std::unexpected{ std::errc::file_exists };
        }

        // NOTE: we can't really make a symlink on android from adb (unless rooted device iirc), so this
        // operation actuall not creating any link on the adb device, just on the in-memory filetree.

        // dummy stat for symlink based on
        // lrw-r--r-- 1 root root 21 2024-10-05 09:19:29.000000000 +0700 /sdcard -> /storage/self/primary
        auto dummy_stat = data::Stat{
            .mode  = S_IFLNK | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,    // mode: "lrw-r--r--"
            .links = 1,
            .uid   = 0,
            .gid   = 0,
            .size  = 21,
            .mtime = Clock::to_time_t(Clock::now()),
        };

        auto node = std::make_unique<Node>(name, this, std::move(dummy_stat), Link{ target });
        auto res  = dir->insert(std::move(node), false);

        return res.and_then([&](auto&& overwrite) {
            assert(not overwrite);
            return dir->find(name);
        });
    }

    Expect<void> Node::rm(Str name, bool recursive, Context context)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        return dir->find(name).and_then([&](Node* node) -> Expect<void> {
            if (node->is<Directory>() and not recursive) {
                return std::unexpected{ std::errc::is_a_directory };
            } else if (auto* file = node->as<RegularFile>()) {
                if (auto res = context.cache.remove(context.connection, file->id()); not res.has_value()) {
                    return std::unexpected{ res.error() };
                }
            }

            auto success = dir->erase(name);
            assert(success);

            return context.connection.rm(context.path, recursive);
        });
    }

    Expect<void> Node::rmdir(Str name, Context context)
    {
        auto* dir = as<Directory>();
        if (dir == nullptr) {
            return std::unexpected{ std::errc::not_a_directory };
        }

        return dir->find(name).and_then([&](Node* node) -> Expect<void> {
            auto* target = node->as<Directory>();
            if (target == nullptr) {
                return std::unexpected{ std::errc::not_a_directory };
            } else if (not target->children().empty()) {
                return std::unexpected{ std::errc::directory_not_empty };
            }
            return context.connection.rmdir(context.path).transform([&] { dir->erase(name); });
        });
    }

    Expect<void> Node::truncate(Context context, off_t size)
    {
        RegularFile* file = nullptr;

        // clang-format off
        if      (is<Directory>()) return std::unexpected{ std::errc::is_a_directory };
        else if (is<Other>())     return std::unexpected{ std::errc::permission_denied };
        else if (is<Link>())      return as<Link>()->real_path()->truncate(context, size);
        else                      file = as<RegularFile>();
        // clang-format on

        auto path = context.cache.get(file->id());
        if (not path.has_value()) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return std::unexpected{ id.error() };
            }
            path = context.cache.get(*id);
            file->set_id(*id);
        }

        assert(std::filesystem::exists(*path));
        context.cache.set_dirty(file->id(), true);

        auto ret = ::truncate(path->c_str(), size);
        if (ret < 0) {
            return std::unexpected{ static_cast<std::errc>(errno) };
        }

        m_stat.size -= size;
        return {};
    }

    Expect<i32> Node::open(Context context, int flags)
    {
        RegularFile* file = nullptr;

        // clang-format off
        if      (is<Directory>()) return std::unexpected{ std::errc::is_a_directory };
        else if (is<Other>())     return std::unexpected{ std::errc::permission_denied };
        else if (is<Link>())      return as<Link>()->real_path()->open(context, flags);
        else                      file = as<RegularFile>();
        // clang-format on

        auto path = context.cache.get(file->id());
        if (not path.has_value()) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return std::unexpected{ id.error() };
            }
            path = context.cache.get(*id);
            file->set_id(*id);
        }

        assert(std::filesystem::exists(*path));
        auto ret = ::open(path->c_str(), flags);

        if (ret >= 0) {
            file->set_fd(ret);
        }

        return std::unexpected{ ret < 0 ? static_cast<std::errc>(errno) : std::errc{} };
    }

    Expect<usize> Node::read(Context context, std::span<char> out, off_t offset)
    {
        RegularFile* file = nullptr;

        // clang-format off
        if      (is<Directory>()) return std::unexpected{ std::errc::is_a_directory };
        else if (is<Other>())     return std::unexpected{ std::errc::permission_denied };
        else if (is<Link>())      return as<Link>()->real_path()->read(context, out, offset);
        else                      file = as<RegularFile>();
        // clang-format on

        // in-case the cache is missing, add it back
        if (not context.cache.exists(file->id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return std::unexpected{ id.error() };
            }
            file->set_id(*id);
        }

        if (auto ret = ::pread(file->fd(), out.data(), out.size(), offset); ret >= 0) {
            return ret;
        }
        return std::unexpected{ static_cast<std::errc>(errno) };
    }

    Expect<usize> Node::write(Context context, std::string_view in, off_t offset)
    {
        RegularFile* file = nullptr;

        // clang-format off
        if      (is<Directory>()) return std::unexpected{ std::errc::is_a_directory };
        else if (is<Other>())     return std::unexpected{ std::errc::permission_denied };
        else if (is<Link>())      return as<Link>()->real_path()->write(context, in, offset);
        else                      file = as<RegularFile>();
        // clang-format on

        // in-case the cache is missing, add it back and set to dirty
        if (not context.cache.exists(file->id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return std::unexpected{ id.error() };
            }
            file->set_id(*id);
        }

        if (auto ret = ::pwrite(file->fd(), (void*)in.data(), in.size(), offset); ret >= 0) {
            context.cache.set_dirty(file->id(), true);
            m_stat.size += ret;
            return ret;
        }
        return std::unexpected{ static_cast<std::errc>(errno) };
    }

    Expect<void> Node::flush(Context context)
    {
        RegularFile* file = nullptr;

        // clang-format off
        if      (is<Directory>()) return std::unexpected{ std::errc::is_a_directory };
        else if (is<Other>())     return std::unexpected{ std::errc::permission_denied };
        else if (is<Link>())      return as<Link>()->real_path()->flush(context);
        else                      file = as<RegularFile>();
        // clang-format on

        // in-case the cache is missing, add it back
        if (not context.cache.exists(file->id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return std::unexpected{ id.error() };
            }
            file->set_id(*id);
        }

        auto ret = ::fsync(file->fd());
        return std::unexpected{ ret < 0 ? static_cast<std::errc>(errno) : std::errc{} };
    }

    Expect<void> Node::release(Context context)
    {
        RegularFile* file = nullptr;

        // clang-format off
        if      (is<Directory>()) return std::unexpected{ std::errc::is_a_directory };
        else if (is<Other>())     return std::unexpected{ std::errc::permission_denied };
        else if (is<Link>())      return as<Link>()->real_path()->release(context);
        else                      file = as<RegularFile>();
        // clang-format on

        // in-case the cache is missing
        // TODO: should have assert that file exist
        if (not context.cache.exists(file->id())) {
            auto id = context.cache.add(context.connection, context.path);
            if (not id.has_value()) {
                return std::unexpected{ id.error() };
            }
            file->set_id(*id);
        }

        auto ret = ::close(file->fd());
        if (auto res = context.cache.flush(context.connection, file->id()); not res.has_value()) {
            return std::unexpected{ res.error() };
        }
        context.cache.set_dirty(file->id(), false);

        return std::unexpected{ ret < 0 ? static_cast<std::errc>(errno) : std::errc{} };
    }

    Expect<void> Node::utimens(Context context)
    {
        return context.connection.touch(context.path, false)
            .and_then([&] { return context.connection.stat(context.path); })
            .transform([&](data::ParsedStat&& parsed) { m_stat = parsed.stat; });
    }
}
