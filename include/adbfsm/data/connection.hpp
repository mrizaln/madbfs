#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

namespace adbfsm::path
{
    class Path;
}

namespace adbfsm::data
{
    struct ParsedStat
    {
        Stat stat;
        Str  path;       // may contains escaped characters
        Str  link_to;    // may be empty and may contains escaped characters
    };

    // NOTE: I made this interface to make Node and FileTree testable
    class IConnection
    {
    public:
        virtual Expect<Gen<ParsedStat>> statdir(path::Path path) = 0;
        virtual Expect<ParsedStat>      stat(path::Path path)    = 0;

        // directory operations
        virtual Expect<void> mkdir(path::Path path)              = 0;
        virtual Expect<void> rm(path::Path path, bool recursive) = 0;
        virtual Expect<void> rmdir(path::Path path)              = 0;
        virtual Expect<void> mv(path::Path from, path::Path to)  = 0;

        // file operations
        virtual Expect<void>  truncate(path::Path path, off_t size)               = 0;
        virtual Expect<Id>    open(path::Path path, int flags)                    = 0;
        virtual Expect<usize> read(path::Path path, Span<char> out, off_t offset) = 0;
        virtual Expect<usize> write(path::Path path, Str in, off_t offset)        = 0;
        virtual Expect<void>  flush(path::Path path)                              = 0;
        virtual Expect<void>  release(path::Path path)                            = 0;

        // directory operation (adding file) or file operation (update time)
        virtual Expect<void> touch(path::Path path, bool create) = 0;

        virtual ~IConnection() = default;
    };

    class Connection : public IConnection
    {
    public:
        // directory operations
        // --------------------

        /**
         * @brief List
         *
         * @param path Path to a directory.
         *
         * @return A generator if successful, or an error if it fails.
         */
        Expect<Gen<ParsedStat>> statdir(path::Path path) override;

        /**
         * @brief Get the stat of a file or directory.
         *
         * @param path The path to the file or directory.
         *
         * @return The stat of the file or directory.
         */
        Expect<ParsedStat> stat(path::Path path) override;

        /**
         * @brief Make a directory on the device.
         *
         * @param path Path to the directory.
         */
        Expect<void> mkdir(path::Path path) override;

        /**
         * @brief Remove a file on the device.
         *
         * @param path Path to the file on the device.
         * @param bool Whether to remove recursively or not.
         */
        Expect<void> rm(path::Path path, bool recursive) override;

        /**
         * @brief Remove a directory on the device.
         *
         * @param path Path to the directory on the device.
         */
        Expect<void> rmdir(path::Path path) override;

        /**
         * @brief Move a file on the device.
         *
         * @param from Target file.
         * @param to Destination file.
         */
        Expect<void> mv(path::Path from, path::Path to) override;

        // --------------------

        // file operations
        // ---------------

        /**
         * @brief Truncate a file on the device.
         *
         * @param path Path to the file on the device.
         * @param size Size to truncate to.
         */
        Expect<void> truncate(path::Path path, off_t size) override;

        /**
         * @brief Open a file on the device.
         *
         * @param path Path to the file on the device.
         * @param flags Flags to open the file with.
         */
        Expect<Id> open(path::Path path, int flags) override;

        /**
         * @brief Read from a file on the device.
         *
         * @param path Path to the file on the device.
         * @param out Buffer to read into.
         * @param offset Offset to read from.
         */
        Expect<usize> read(path::Path path, Span<char> out, off_t offset) override;

        /**
         * @brief Write to a file on the device.
         *
         * @param path Path to the file on the device.
         * @param in Buffer to write from.
         * @param offset Offset to write to.
         */
        Expect<usize> write(path::Path path, Str in, off_t offset) override;

        /**
         * @brief Flush a file on the device.
         *
         * @param path Path to the file on the device.
         */
        Expect<void> flush(path::Path path) override;

        /**
         * @brief Release a file on the device.
         *
         * @param path Path to the file on the device.
         */
        Expect<void> release(path::Path path) override;

        // ---------------

        /**
         * @brief Touch a file on the device.
         *
         * @param path Path to the file.
         * @param create Indicate whether to create a file if not exist.
         */
        Expect<void> touch(path::Path path, bool create) override;
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
    Expect<void> start_connection();

    /**
     * @brief List connected devices.
     */
    Expect<Vec<Device>> list_devices();
}
