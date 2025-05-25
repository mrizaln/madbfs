#pragma once

#include "madbfs/common.hpp"
#include "madbfs/data/stat.hpp"

#include <madbfs/async/async.hpp>

namespace madbfs::path
{
    class Path;
    class PathBuf;
}

namespace madbfs::data
{
    struct ParsedStat
    {
        Stat stat;
        Str  path;
    };

    // NOTE: I made this interface to make Node and FileTree testable
    class IConnection
    {
    public:
        virtual AExpect<Gen<ParsedStat>> statdir(path::Path path)  = 0;
        virtual AExpect<Stat>            stat(path::Path path)     = 0;
        virtual AExpect<path::PathBuf>   readlink(path::Path path) = 0;

        // directory operations
        virtual AExpect<void> mkdir(path::Path path)              = 0;
        virtual AExpect<void> rm(path::Path path, bool recursive) = 0;
        virtual AExpect<void> rmdir(path::Path path)              = 0;
        virtual AExpect<void> mv(path::Path from, path::Path to)  = 0;

        // file operations
        virtual AExpect<void>  truncate(path::Path path, off_t size)                     = 0;
        virtual AExpect<u64>   open(path::Path path, int flags)                          = 0;
        virtual AExpect<usize> read(path::Path path, Span<char> out, off_t offset)       = 0;
        virtual AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) = 0;
        virtual AExpect<void>  flush(path::Path path)                                    = 0;
        virtual AExpect<void>  release(path::Path path)                                  = 0;

        virtual AExpect<usize> copy_file_range(
            path::Path in,
            off_t      in_off,
            path::Path out,
            off_t      out_off,
            usize      size
        ) = 0;

        // directory operation (adding file) or file operation (update time)
        virtual AExpect<void> touch(path::Path path, bool create) = 0;

        virtual ~IConnection() = default;
    };

    class Connection final : public IConnection
    {
    public:
        Connection() = default;

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
        AExpect<Stat> stat(path::Path path) override;

        /**
         * @brief Get the real file pointed by a symlink.
         *
         * @param path The path to the file or directory.
         */
        AExpect<path::PathBuf> readlink(path::Path path) override;

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
        AExpect<void> rm(path::Path path, bool recursive) override;

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
        AExpect<void> mv(path::Path from, path::Path to) override;

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
         * @brief Open a file on the device.
         *
         * @param path Path to the file on the device.
         * @param flags Flags to open the file with.
         */
        AExpect<u64> open(path::Path path, int flags) override;

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
         * @brief Flush a file on the device.
         *
         * @param path Path to the file on the device.
         */
        AExpect<void> flush(path::Path path) override;

        /**
         * @brief Release a file on the device.
         *
         * @param path Path to the file on the device.
         */
        AExpect<void> release(path::Path path) override;

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

        /**
         * @brief Touch a file on the device.
         *
         * @param path Path to the file.
         * @param create Indicate whether to create a file if not exist.
         */
        AExpect<void> touch(path::Path path, bool create) override;

    private:
        std::atomic<u64> m_counter = 0;
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

    constexpr Str to_string(DeviceStatus status)
    {
        switch (status) {
        case DeviceStatus::Device: return "device ok";
        case DeviceStatus::Offline: return "device offline";
        case DeviceStatus::Unauthorized: return "device unauthorized";
        case DeviceStatus::Unknown: return "unknown";
        }
        return "Unknown";
    }

    /**
     * @brief Start connection with the devices.
     */
    AExpect<void> start_connection();

    /**
     * @brief List connected devices.
     */
    AExpect<Vec<Device>> list_devices();
}
