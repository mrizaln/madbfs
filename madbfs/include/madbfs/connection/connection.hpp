#pragma once

#include "madbfs/data/stat.hpp"

#include <madbfs-common/async/async.hpp>

namespace madbfs::path
{
    class Path;
    class PathBuf;
}

namespace madbfs::connection
{
    struct ParsedStat
    {
        data::Stat stat;
        Str        path;
    };

    class Connection
    {
    public:
        virtual Str name() const = 0;

        // directory operations
        // --------------------

        /**
         * @brief List
         *
         * @param path Path to a directory.
         *
         * @return A generator if successful, or an error if it fails.
         */
        virtual AExpect<Gen<ParsedStat>> statdir(path::Path path) = 0;

        /**
         * @brief Get the stat of a file or directory.
         *
         * @param path The path to the file or directory.
         *
         * @return The stat of the file or directory.
         */
        virtual AExpect<data::Stat> stat(path::Path path) = 0;

        /**
         * @brief Get the real file pointed by a symlink.
         *
         * @param path The path to the file or directory.
         *
         * The returned path may be relative so it's not safe to directly convert it to `path::path` or
         * `path::PathBuf`, use `path::resolve()` instead in that case.
         */
        virtual AExpect<String> readlink(path::Path path) = 0;

        /**
         * @brief Create a new empty file.
         *
         * @param path Path of the new file.
         */
        virtual AExpect<void> mknod(path::Path path, mode_t mode, dev_t dev) = 0;

        /**
         * @brief Make a directory on the device.
         *
         * @param path Path to the directory.
         */
        virtual AExpect<void> mkdir(path::Path path, mode_t mode) = 0;

        /**
         * @brief Remove a file on the device.
         *
         * @param path Path to the file on the device.
         * @param bool Whether to remove recursively or not.
         */
        virtual AExpect<void> unlink(path::Path path) = 0;

        /**
         * @brief Remove a directory on the device.
         *
         * @param path Path to the directory on the device.
         */
        virtual AExpect<void> rmdir(path::Path path) = 0;

        /**
         * @brief Move a file on the device.
         *
         * @param from Target file.
         * @param to Destination file.
         */
        virtual AExpect<void> rename(path::Path from, path::Path to, u32 flags) = 0;

        // --------------------

        // file operations
        // ---------------

        /**
         * @brief Truncate a file on the device.
         *
         * @param path Path to the file on the device.
         * @param size Size to truncate to.
         */
        virtual AExpect<void> truncate(path::Path path, off_t size) = 0;

        /**
         * @brief Read from a file on the device.
         *
         * @param path Path to the file on the device.
         * @param out Buffer to read into.
         * @param offset Offset to read from.
         */
        virtual AExpect<usize> read(path::Path path, Span<char> out, off_t offset) = 0;

        /**
         * @brief Write to a file on the device.
         *
         * @param path Path to the file on the device.
         * @param in Buffer to write from.
         * @param offset Offset to write to.
         */
        virtual AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) = 0;

        /**
         * @brief Update change time and modification time of a file
         *
         * @param path Path to the file on the device.
         * @param atime Access time.
         * @param mtime Modification time.
         */
        virtual AExpect<void> utimens(path::Path path, timespec atime, timespec mtime) = 0;

        /**
         * @brief Copy file server-side.
         *
         * @param in Input file path.
         * @param in_off Input offset.
         * @param out Output file path.
         * @param out_off Output offset.
         * @param size Number of bytes to be copied.
         */
        virtual AExpect<usize> copy_file_range(
            path::Path in,
            off_t      in_off,
            path::Path out,
            off_t      out_off,
            usize      size
        ) = 0;

        // ---------------

        virtual ~Connection() = default;
    };

    enum class DeviceStatus
    {
        Device,
        Offline,
        Unauthorized,
        Unknown,
    };

    struct Device
    {
        String       serial;
        DeviceStatus status;
    };

    /**
     * @brief Get human readable description of DeviceStatus.
     */
    Str to_string(DeviceStatus status);

    /**
     * @brief Start connection with the devices.
     */
    AExpect<void> start_connection();

    /**
     * @brief List connected devices.
     */
    AExpect<Vec<Device>> list_devices();
}
