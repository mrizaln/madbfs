#pragma once

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>

namespace madbfs::adb
{
    /**
     * @enum DeviceStatus
     *
     * @brief Represent the status of device connected through adb.
     */
    enum class DeviceStatus
    {
        Device,
        Emulator,
        Offline,
        Unauthorized,
        Unknown,
    };

    /**
     * @class Device
     *
     * @brief Device information connected through adb.
     */
    struct Device
    {
        String       serial;
        DeviceStatus status;
    };

    /**
     * @brief Get human readable description of DeviceStatus.
     *
     * The string lifetime is static.
     */
    Str to_string(DeviceStatus status);

    /**
     * @brief Start connection with the devices.
     *
     * Starts adb server.
     */
    AExpect<void> start_server();

    /**
     * @brief List connected devices.
     */
    AExpect<Vec<Device>> list_devices();
}
