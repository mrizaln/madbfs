#pragma once

#include "madbfs/connection/connection.hpp"

namespace madbfs::connection
{
    class AdbConnection final : public Connection
    {
    public:
        AdbConnection() = default;

        // directory operations
        // --------------------

        /**
         * @brief List
         *
         * @param path Path to a directory.
         *
         * @return A generator if successful, or an error if it fails.
         */
        AExpect<Gen<ParsedStat>> statdir(path::Path path) override;

        /**
         * @brief Get the stat of a file or directory.
         *
         * @param path The path to the file or directory.
         *
         * @return The stat of the file or directory.
         */
        AExpect<data::Stat> stat(path::Path path) override;

        /**
         * @brief Get the real file pointed by a symlink.
         *
         * @param path The path to the file or directory.
         */
        AExpect<path::PathBuf> readlink(path::Path path) override;

        /**
         * @brief Create a new empty file.
         *
         * @param path Path of the new file.
         */
        AExpect<void> mknod(path::Path path) override;

        /**
         * @brief Make a directory on the device.
         *
         * @param path Path to the directory.
         */
        AExpect<void> mkdir(path::Path path) override;

        /**
         * @brief Remove a file on the device.
         *
         * @param path Path to the file on the device.
         * @param bool Whether to remove recursively or not.
         */
        AExpect<void> unlink(path::Path path) override;

        /**
         * @brief Remove a directory on the device.
         *
         * @param path Path to the directory on the device.
         */
        AExpect<void> rmdir(path::Path path) override;

        /**
         * @brief Move a file on the device.
         *
         * @param from Target file.
         * @param to Destination file.
         */
        AExpect<void> rename(path::Path from, path::Path to) override;

        // --------------------

        // file operations
        // ---------------

        /**
         * @brief Truncate a file on the device.
         *
         * @param path Path to the file on the device.
         * @param size Size to truncate to.
         */
        AExpect<void> truncate(path::Path path, off_t size) override;

        /**
         * @brief Read from a file on the device.
         *
         * @param path Path to the file on the device.
         * @param out Buffer to read into.
         * @param offset Offset to read from.
         */
        AExpect<usize> read(path::Path path, Span<char> out, off_t offset) override;

        /**
         * @brief Write to a file on the device.
         *
         * @param path Path to the file on the device.
         * @param in Buffer to write from.
         * @param offset Offset to write to.
         */
        AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) override;

        /**
         * @brief Update change time and modification time of a file
         *
         * @param path Path to the file on the device.
         * @param atime Access time.
         * @param mtime Modification time.
         */
        AExpect<void> utimens(path::Path path, timespec atime, timespec mtime) override;

        /**
         * @brief Copy file server-side.
         *
         * @param in Input file path.
         * @param in_off Input offset.
         * @param out Output file path.
         * @param out_off Output offset.
         * @param size Number of bytes to be copied.
         */
        AExpect<usize> copy_file_range(path::Path in, off_t in_off, path::Path out, off_t out_off, usize size)
            override;

        // ---------------
    };
}
