#include "madbfs/node.hpp"

#include "madbfs/connection.hpp"

namespace madbfs::node
{
    u64 Directory::NodeHash::operator()(const Uniq<Node>& node) const
    {
        return (*this)(node->name());
    }

    bool Directory::NodeEq::operator()(Str lhs, const Uniq<Node>& rhs) const
    {
        return lhs == rhs->name();
    }

    bool Directory::NodeEq::operator()(const Uniq<Node>& lhs, Str rhs) const
    {
        return lhs->name() == rhs;
    }

    bool Directory::NodeEq::operator()(const Uniq<Node>& lhs, const Uniq<Node>& rhs) const
    {
        return lhs->name() == rhs->name();
    }

    Expect<Ref<Node>> Directory::find(Str name) const
    {
        if (auto found = m_children.find(name); found != m_children.end()) {
            return *found->get();
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    Opt<Uniq<Node>> Directory::erase(Str name)
    {
        if (auto found = m_children.find(name); found != m_children.end()) {
            return std::move(m_children.extract(found).value());
        }
        return std::nullopt;
    }

    Expect<Pair<Ref<Node>, Uniq<Node>>> Directory::insert(Uniq<Node> node, bool overwrite)
    {
        auto found = m_children.find(node->name());
        if (found != m_children.end() and not overwrite) {
            return Unexpect{ Errc::file_exists };
        }

        auto released = Uniq<Node>{};
        if (found != m_children.end()) {
            released = std::move(m_children.extract(found).value());
        }

        auto [back, _] = m_children.emplace(std::move(node));
        return Pair{ std::ref(*back->get()), std::move(released) };
    }

    Expect<Uniq<Node>> Directory::extract(Str name)
    {
        if (auto found = m_children.find(name); found != m_children.end()) {
            return std::move(m_children.extract(found).value());
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }
}

namespace madbfs
{
    void Node::expires_after(Seconds duration)
    {
        if (duration == Seconds::max()) {
            m_expiration = Timepoint::max();
        } else {
            m_expiration = SteadyClock::now() + duration;
        }
    }

    bool Node::expired() const
    {
        // prevent expiration if the file is dirty
        if (auto file = as_regular(); file and file->get().dirty) {
            return false;
        }
        return SteadyClock::now() > m_expiration;
    }

    // TODO: maybe preserve open file handles? it will be hard though, I may need to do it recursively if
    // directory is changed, idk
    File Node::mutate(File file)
    {
        return std::exchange(m_value, std::move(file));
    }

    const node::Error* Node::as_error() const
    {
        return std::get_if<node::Error>(&m_value);
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

    void Node::refresh_stat(timespec atime, timespec mtime)
    {
        auto now  = SystemClock::now().time_since_epoch();
        auto sec  = std::chrono::duration_cast<Seconds>(now);
        auto nsec = std::chrono::duration_cast<Nanoseconds>(now - sec);

        auto time = timespec{ .tv_sec = sec.count(), .tv_nsec = nsec.count() };

        if (atime.tv_nsec != UTIME_OMIT) {
            m_stat.atime = atime.tv_nsec == UTIME_NOW ? time : atime;
        }
        if (mtime.tv_nsec != UTIME_OMIT) {
            m_stat.mtime = mtime.tv_nsec == UTIME_NOW ? time : mtime;
        }
        m_stat.ctime = time;
    }

    bool Node::has_synced() const
    {
        auto visit = Overload{
            [](const node::Directory& dir) { return dir.has_readdir(); },
            [](const auto&) { return true; },
        };
        return std::visit(visit, m_value);
    }

    void Node::set_synced(bool synced)
    {
        std::ignore = as_directory().transform(proj(&node::Directory::set_readdir, synced));
    }

    Expect<Ref<Node>> Node::traverse(Str name) const
    {
        return as_directory().and_then(proj(&node::Directory::find, name));
    }

    Expect<Ref<Node>> Node::build(Str name, Stat stat, File file)
    {
        return as_directory()
            .and_then(proj(
                &node::Directory::insert,
                std::make_unique<Node>(name, this, std::move(stat), std::move(file)),
                false
            ))
            .transform([](auto&& pair) { return pair.first; });
    }
}
