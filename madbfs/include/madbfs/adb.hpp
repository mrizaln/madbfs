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
     * @enum Abi
     *
     * @brief Represent phone ABI.
     */
    enum class Abi
    {
        Armeabi_v7a,
        Arm64_v8a,
        X86,
        X86_64,
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
     * @brief Get human readable description of Abi.
     *
     * The string lifetime is static.
     */
    Str to_string(Abi abi);

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

    /**
     * @brief Get ABI of device with specified serial.
     *
     * @param serial Device serial.
     */
    AExpect<Abi> get_abi(Str serial);
}
