#include "madbfs/tree/node.hpp"

// node.hpp impl: Directory
namespace madbfs::tree::node
{
    Expect<Id> Directory::find(Str name) const
    {
        if (auto found = m_children.find(name); found != m_children.end()) {
            return found->second;
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    Expect<Id> Directory::erase(Str name)
    {
        if (auto value = m_children.extract(name); not value.empty()) {
            return std::move(value).mapped();
        }
        return Unexpect{ Errc::no_such_file_or_directory };
    }

    Expect<Opt<Id>> Directory::insert(Str name, Id node, bool overwrite)
    {
        auto found = m_children.find(name);
        if (found != m_children.end() and not overwrite) {
            return Unexpect{ Errc::file_exists };
        }

        auto extract = Opt<Id>{};
        if (found != m_children.end()) {
            extract = std::move(m_children.extract(found).mapped());
        }

        m_children.emplace(name, node);
        return extract;
    }
}

// node.hpp impl: Node
namespace madbfs::tree
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

    Expect<Id> Node::traverse(Str name) const
    {
        return as_directory().and_then(proj(&node::Directory::find, name));
    }

    Expect<void> Node::add(Str name, Id node)
    {
        return as_directory()
            .and_then(proj(&node::Directory::insert, name, node, false))
            .transform(sink_void);
    }

    struct stat* Node::fill_stbuf(struct stat* stbuf, Id id, blksize_t page_size)
    {
        std::memset(stbuf, 0, sizeof(struct stat));

        stbuf->st_ino     = id.to_u64();
        stbuf->st_mode    = m_stat.mode;
        stbuf->st_nlink   = m_stat.links;
        stbuf->st_uid     = m_stat.uid;
        stbuf->st_gid     = m_stat.gid;
        stbuf->st_size    = m_stat.size;
        stbuf->st_blksize = page_size;
        stbuf->st_blocks  = (stbuf->st_size + 511) / 512;    // strictly in 512 B units [read stat(3)]
        stbuf->st_atim    = m_stat.atime;
        stbuf->st_mtim    = m_stat.mtime;
        stbuf->st_ctim    = m_stat.ctime;

        return stbuf;
    }
}
