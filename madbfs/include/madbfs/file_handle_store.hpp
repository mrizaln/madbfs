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
        u64      real_fd;    // only useful for direct IO; Cache is not enabled
    };

    /**
     * @class FileHandleStore
     *
     * @brief Storage for open file handles.
     *
     * The lifetime of the nodes pointed to by this store should be at the very least from `store()` to
     * `release()`. If you can't guarantee that the node pointed to can't live til `release()`, `erase()`
     * them.
     */
    class FileHandleStore
    {
    public:
        /**
         * @brief Get file handle from file handle store for file descriptor.
         *
         * @param fd File descriptor.
         *
         * @return FileHandle if exists, else `std::nullopt`.
         *
         * The time complexity of the operation is constant.
         */
        Opt<FileHandle> find(u64 fd);

        /**
         * @brief Get associated `Node` from file handle store for file descriptor with specified open mode.
         *
         * @param fd File descriptor.
         * @param mode Open file mode.
         *
         * @return The node if found and fulfill the mode else `std::nullopt`.
         *
         * The time complexity of the operation is constant.
         */
        Opt<FileHandle> find(u64 fd, OpenMode mode);

        /**
         * @brief Store `Node` pointer into file handle store.
         *
         * @param node The node to be inserted.
         * @param mode Open file mode for the node.
         * @param real_fd Real file descriptor (only useful for direct IO).
         *
         * @return File descriptor (position of the node in the store).
         *
         * The time complexity of the opration is linear (depends on number of handles before finding a hole).
         */
        u64 store(Node* node, OpenMode mode, u64 real_fd);

        /**
         * @brief Release the associated node of file descriptor from the file handle store.
         *
         * @param fd File descriptor.
         *
         * @return The released node if exists, else `std::nullopt`.
         *
         * The time complexity of the operation is constant.
         */
        Opt<FileHandle> release(u64 fd);

        /**
         * @brief Erase any pointer to node.
         *
         * @param node The pointer to the node.
         *
         * @return Number of file handle erased.
         *
         * Iterate the store and erase any handles that has this node pointed by them.
         */
        usize erase(Node* node);

        Span<FileHandle>       iter() { return m_handles; }
        Span<const FileHandle> iter() const { return m_handles; }

        usize capacity() const { return m_handles.size(); }
        usize count_open() const;
        usize count_empty() const;

    private:
        Vec<FileHandle> m_handles;
    };
}
