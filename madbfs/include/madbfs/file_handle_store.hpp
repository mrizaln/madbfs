#pragma once

#include "madbfs/stat.hpp"

namespace madbfs
{
    class Node;
}

namespace madbfs
{
    struct FileHandle
    {
        Node*    node;
        OpenMode mode;
    };

    class FileHandleStore
    {
    public:
        /**
         * @brief Get file handle from file handle store for file descriptor.
         *
         * @param fd File descriptor.
         *
         * @return FileHandle if exists, else default initialized FileHandle.
         *
         * The complexity of the operation is constant.
         */
        FileHandle find(u64 fd);

        /**
         * @brief Get associated `Node` from file handle store for file descriptor with specified open mode.
         *
         * @param fd File descriptor.
         * @param mode Open file mode.
         *
         * @return The node if found and fulfill the mode else `Errc::bad_file_descriptor`.
         *
         * The complexity of the operation is constant.
         */
        Node* find(u64 fd, OpenMode mode);

        /**
         * @brief Store `Node` pointer into file handle store.
         *
         * @param node The node to be inserted.
         * @param mode Open file mode for the node.
         *
         * @return File descriptor (position of the node in the store).
         *
         * The complexity of the opration is linear (depends on number of handles before finding a hole).
         */
        u64 store(Node* node, OpenMode mode);

        /**
         * @brief Release the associated node of file descriptor from the file handle store.
         *
         * @param fd File descriptor.
         *
         * @return The released node if exists, else default initialized FileHandle.
         *
         * The complexity of the operation is constant.
         */
        FileHandle release(u64 fd);

        FRange auto iter() { return sv::zip(m_nodes, m_modes); }
        FRange auto iter() const { return sv::zip(std::as_const(m_nodes), std::as_const(m_modes)); }

        usize size() const { return m_nodes.size(); }

    private:
        std::vector<Node*>    m_nodes;
        std::vector<OpenMode> m_modes;
    };
}
