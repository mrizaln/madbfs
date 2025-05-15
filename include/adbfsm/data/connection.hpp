#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/data/stat.hpp"

#include <adbfsm/async/async.hpp>

namespace adbfsm::path
{
    class Path;
    class PathBuf;
}

namespace adbfsm::data
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
        template <typename T>
        using Aresult = async::Await<Expect<T>>;

        virtual Aresult<Gen<ParsedStat>> statdir(path::Path path)  = 0;
        virtual Aresult<Stat>            stat(path::Path path)     = 0;
        virtual Aresult<path::PathBuf>   readlink(path::Path path) = 0;

        // directory operations
        virtual Aresult<void> mkdir(path::Path path)              = 0;
        virtual Aresult<void> rm(path::Path path, bool recursive) = 0;
        virtual Aresult<void> rmdir(path::Path path)              = 0;
        virtual Aresult<void> mv(path::Path from, path::Path to)  = 0;

        // file operations
        virtual Aresult<void>  truncate(path::Path path, off_t size)                     = 0;
        virtual Aresult<u64>   open(path::Path path, int flags)                          = 0;
        virtual Aresult<usize> read(path::Path path, Span<char> out, off_t offset)       = 0;
        virtual Aresult<usize> write(path::Path path, Span<const char> in, off_t offset) = 0;
        virtual Aresult<void>  flush(path::Path path)                                    = 0;
        virtual Aresult<void>  release(path::Path path)                                  = 0;

        // directory operation (adding file) or file operation (update time)
        virtual Aresult<void> touch(path::Path path, bool create) = 0;

        virtual ~IConnection() = default;
    };

    class Connection final : public IConnection
    {
    public:
        Connection(async::Context& context, usize page_size)
            : m_context{ context }
            , m_page_size{ page_size }
        {
        }

        // directory operations
        // --------------------

        /**
         * @brief List
         *
         * @param path Path to a directory.
         *
         * @return A generator if successful, or an error if it fails.
         */
        Aresult<Gen<ParsedStat>> statdir(path::Path path) override;

        /**
         * @brief Get the stat of a file or directory.
         *
         * @param path The path to the file or directory.
         *
         * @return The stat of the file or directory.
         */
        Aresult<Stat> stat(path::Path path) override;

        /**
         * @brief Get the real file pointed by a symlink.
         *
         * @param path The path to the file or directory.
         */
        Aresult<path::PathBuf> readlink(path::Path path) override;

        /**
         * @brief Make a directory on the device.
         *
         * @param path Path to the directory.
         */
        Aresult<void> mkdir(path::Path path) override;

        /**
         * @brief Remove a file on the device.
         *
         * @param path Path to the file on the device.
         * @param bool Whether to remove recursively or not.
         */
        Aresult<void> rm(path::Path path, bool recursive) override;

        /**
         * @brief Remove a directory on the device.
         *
         * @param path Path to the directory on the device.
         */
        Aresult<void> rmdir(path::Path path) override;

        /**
         * @brief Move a file on the device.
         *
         * @param from Target file.
         * @param to Destination file.
         */
        Aresult<void> mv(path::Path from, path::Path to) override;

        // --------------------

        // file operations
        // ---------------

        /**
         * @brief Truncate a file on the device.
         *
         * @param path Path to the file on the device.
         * @param size Size to truncate to.
         */
        Aresult<void> truncate(path::Path path, off_t size) override;

        /**
         * @brief Open a file on the device.
         *
         * @param path Path to the file on the device.
         * @param flags Flags to open the file with.
         */
        Aresult<u64> open(path::Path path, int flags) override;

        /**
         * @brief Read from a file on the device.
         *
         * @param path Path to the file on the device.
         * @param out Buffer to read into.
         * @param offset Offset to read from.
         */
        Aresult<usize> read(path::Path path, Span<char> out, off_t offset) override;

        /**
         * @brief Write to a file on the device.
         *
         * @param path Path to the file on the device.
         * @param in Buffer to write from.
         * @param offset Offset to write to.
         */
        Aresult<usize> write(path::Path path, Span<const char> in, off_t offset) override;

        /**
         * @brief Flush a file on the device.
         *
         * @param path Path to the file on the device.
         */
        Aresult<void> flush(path::Path path) override;

        /**
         * @brief Release a file on the device.
         *
         * @param path Path to the file on the device.
         */
        Aresult<void> release(path::Path path) override;

        // ---------------

        /**
         * @brief Touch a file on the device.
         *
         * @param path Path to the file.
         * @param create Indicate whether to create a file if not exist.
         */
        Aresult<void> touch(path::Path path, bool create) override;

    private:
        async::Context&  m_context;
        std::atomic<u64> m_counter   = 0;
        usize            m_page_size = 0;
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
    Connection::Aresult<void> start_connection();

    /**
     * @brief List connected devices.
     */
    Connection::Aresult<Vec<Device>> list_devices();
}
