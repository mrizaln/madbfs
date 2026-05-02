#include "madbfs/file_handle_store.hpp"

#include "madbfs/node.hpp"

namespace madbfs
{
    FileHandle FileHandleStore::find(u64 fd)
    {
        return fd < m_nodes.size() ? FileHandle{ m_nodes[fd], m_modes[fd] } : FileHandle{};
    }

    Node* FileHandleStore::find(u64 fd, data::OpenMode mode)
    {
        if (fd >= m_nodes.size()) {
            return nullptr;
        }

        auto ok   = m_modes[fd] == mode or m_modes[fd] == data::OpenMode::ReadWrite;
        auto node = m_nodes[fd];

        return node and ok ? node : nullptr;
    }

    u64 FileHandleStore::store(Node* node, data::OpenMode mode)
    {
        if (auto found = sr::find(m_nodes, nullptr); found != m_nodes.end()) {
            auto dist     = static_cast<usize>(found - m_nodes.begin());
            m_nodes[dist] = node;
            m_modes[dist] = mode;
            return dist;
        }

        auto size = m_nodes.size();

        if (m_nodes.empty()) {
            m_nodes.resize(1024, {});    // 1024 seats by default is reasonable I guess
            m_modes.resize(1024, {});
        } else {
            m_nodes.resize(size * 2, {});    // should I add upper limit?
            m_modes.resize(size * 2, {});
        }

        m_nodes[size] = node;
        m_modes[size] = mode;

        return size;
    }

    FileHandle FileHandleStore::release(u64 fd)
    {
        return fd < m_nodes.size()
                 ? FileHandle{ std::exchange(m_nodes[fd], {}), std::exchange(m_modes[fd], {}) }
                 : FileHandle{};
    }
}
