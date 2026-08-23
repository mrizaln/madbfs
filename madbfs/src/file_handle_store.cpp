#include "madbfs/file_handle_store.hpp"

// file_handle_store.hpp impl
namespace madbfs
{
    Opt<FileHandle> FileHandleStore::find(u64 fd)
    {
        auto key    = from_u64<Key>(fd);
        auto handle = m_handles.at(key);
        return handle ? Opt{ *handle } : std::nullopt;
    }

    Opt<FileHandle> FileHandleStore::find(u64 fd, OpenMode mode)
    {
        auto key    = from_u64<Key>(fd);
        auto handle = m_handles.at(key);

        if (handle == nullptr or (handle->mode != mode and handle->mode != OpenMode::ReadWrite)) {
            return std::nullopt;
        }

        return *handle;
    }

    u64 FileHandleStore::store(Node* node, OpenMode mode, u64 real_fd)
    {
        assert(node != nullptr);
        auto key = m_handles.emplace(node, mode, real_fd);
        return to_u64(key);
    }

    Opt<FileHandle> FileHandleStore::release(u64 fd)
    {
        auto key = from_u64<Key>(fd);
        return m_handles.erase(key);
    }

    usize FileHandleStore::erase(Node* node)
    {
        assert(node != nullptr);
        return m_handles.erase_if([&](Key, FileHandle& h) { return h.node == node; });
    }
}
