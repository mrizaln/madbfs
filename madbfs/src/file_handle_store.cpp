#include "madbfs/file_handle_store.hpp"

#include "madbfs/node.hpp"

#include <algorithm>
#include <utility>

namespace madbfs
{
    Opt<FileHandle> FileHandleStore::find(u64 fd)
    {
        return fd < m_handles.size() ? Opt{ m_handles[fd] } : std::nullopt;
    }

    Opt<FileHandle> FileHandleStore::find(u64 fd, OpenMode mode)
    {
        if (fd >= m_handles.size()) {
            return std::nullopt;
        }

        auto handle = m_handles[fd];
        auto ok     = handle.mode == mode or handle.mode == OpenMode::ReadWrite;

        return handle.node and ok ? Opt{ handle } : std::nullopt;
    }

    u64 FileHandleStore::store(Node* node, OpenMode mode, u64 real_fd)
    {
        if (auto found = sr::find(m_handles, nullptr, &FileHandle::node); found != m_handles.end()) {
            auto dist               = static_cast<usize>(found - m_handles.begin());
            m_handles[dist].node    = node;
            m_handles[dist].mode    = mode;
            m_handles[dist].real_fd = real_fd;
            return dist;
        }

        auto size = m_handles.size();

        if (m_handles.empty()) {
            m_handles.resize(1024, {});    // 1024 seats by default is reasonable I guess
        } else {
            m_handles.resize(size * 2, {});    // should I add upper limit?
        }

        m_handles[size].node    = node;
        m_handles[size].mode    = mode;
        m_handles[size].real_fd = real_fd;

        return size;
    }

    Opt<FileHandle> FileHandleStore::release(u64 fd)
    {
        return fd < m_handles.size() ? Opt{ std::exchange(m_handles[fd], {}) } : std::nullopt;
    }

    usize FileHandleStore::erase(Node* node)
    {
        auto count = 0uz;
        for (auto& v : m_handles) {
            if (v.node == node) {
                v.node = nullptr;
                ++count;
            }
        }
        return count;
    }

    usize FileHandleStore::count_open() const
    {
        auto count = sr::count_if(m_handles, [](const FileHandle& h) { return h.node != nullptr; });
        return static_cast<usize>(count);
    }

    usize FileHandleStore::count_empty() const
    {
        return capacity() - count_open();
    }
}
