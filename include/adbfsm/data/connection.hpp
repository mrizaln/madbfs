#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

#include <generator>

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
        virtual Expect<std::generator<ParsedStat>> stat_dir(path::Path path)            = 0;
        virtual Expect<ParsedStat>                 stat(path::Path path)                = 0;
        virtual Expect<void>                       touch(path::Path path, bool create)  = 0;
        virtual Expect<void>                       mkdir(path::Path path)               = 0;
        virtual Expect<void>                       rm(path::Path path, bool recursive)  = 0;
        virtual Expect<void>                       rmdir(path::Path path)               = 0;
        virtual Expect<void>                       mv(path::Path from, path::Path to)   = 0;
        virtual Expect<void>                       pull(path::Path from, path::Path to) = 0;
        virtual Expect<void>                       push(path::Path from, path::Path to) = 0;

        virtual ~IConnection() = default;
    };

    class Connection : public IConnection
    {
    public:
        /**
         * @brief List
         *
         * @param path Path to a directory.
         *
         * @return A generator if successful, or an error if it fails.
         */
        Expect<std::generator<ParsedStat>> stat_dir(path::Path path) override;

        /**
         * @brief Get the stat of a file or directory.
         *
         * @param path The path to the file or directory.
         *
         * @return The stat of the file or directory.
         */
        Expect<ParsedStat> stat(path::Path path) override;

        /**
         * @brief Touch a file on the device.
         *
         * @param path Path to the file.
         * @param create Indicate whether to create a file if not exist.
         */
        Expect<void> touch(path::Path path, bool create) override;

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

        /**
         * @brief Pull a file from the device to local.
         *
         * @param from Path to the file on the device.
         * @param to Path to the file destination on local.
         */
        Expect<void> pull(path::Path from, path::Path to) override;

        /**
         * @brief Push a file from local to the device.
         *
         * @param from Path to the file on local.
         * @param to Path to the file destination on the device.
         */
        Expect<void> push(path::Path from, path::Path to) override;
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

    constexpr std::string_view to_string(DeviceStatus status)
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
    Expect<std::vector<Device>> list_devices();

}
