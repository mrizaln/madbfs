#pragma once

#include "madbfs/slotmap.hpp"
#include "madbfs/stat.hpp"

namespace madbfs
{
    // TODO: store id instead
    struct FileHandle
    {
        Id       id;
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
         * @brief Get associated `FileHandle` for file descriptor with any open mode.
         *
         * @param fd File descriptor.
         *
         * @return FileHandle if exists, else `std::nullopt`.
         *
         * The time complexity of the operation is constant.
         */
        Opt<FileHandle> find(u64 fd);

        /**
         * @brief Get associated `FileHandle` for file descriptor with specified open mode.
         *
         * @param fd File descriptor.
         * @param mode Open file mode.
         *
         * @return The node if found and fulfill the mode else `std::nullopt`.
         */
        Opt<FileHandle> find(u64 fd, OpenMode mode);

        /**
         * @brief Create a new `FileHandle` associated by a file descriptor number.
         *
         * @param id The id of the node of the file to be referenced by the handle.
         * @param mode Open file mode for the node.
         * @param real_fd Real file descriptor (only useful for direct IO).
         *
         * @return File descriptor.
         */
        u64 store(Id id, OpenMode mode, u64 real_fd);

        /**
         * @brief Release the associated file handle using the file descriptor number.
         *
         * @param fd File descriptor.
         *
         * @return The released node if exists, else `std::nullopt`.
         */
        Opt<FileHandle> release(u64 fd);

        /**
         * @brief Erase any pointer to node id.
         *
         * @param id Unique identifier for file node.
         *
         * @return Number of file handle erased.
         *
         * Iterate the store and erase any handles that has this node pointed by them.
         */
        usize erase(Id id);

        usize capacity() const { return m_handles.capacity(); }
        usize size() const { return m_handles.size(); }

    private:
        struct Key
        {
            u32 index;
            u32 version;
        };

        SlotMap<Key, FileHandle> m_handles;
    };
}
