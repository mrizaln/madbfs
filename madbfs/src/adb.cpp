#include "madbfs/adb.hpp"

#include "madbfs/cmd.hpp"

#include <madbfs-common/util/split.hpp>

// adb.hpp impl
namespace madbfs::adb
{
    Str to_string(DeviceStatus status)
    {
        switch (status) {
        case DeviceStatus::Device: return "device";
        case DeviceStatus::Emulator: return "emulator";
        case DeviceStatus::Offline: return "offline";
        case DeviceStatus::Unauthorized: return "unauthorized";
        case DeviceStatus::Unknown: return "unknown";
        }
        return "Unknown";
    }

    Str to_string(Abi abi)
    {
        switch (abi) {
        case Abi::Armeabi_v7a: return "armeabi-v7a";
        case Abi::Arm64_v8a: return "arm64-v8a";
        case Abi::X86: return "x86";
        case Abi::X86_64: return "x86_64";
        }
        return "unknown";
    }

    AExpect<void> start_server()
    {
        auto res = co_await cmd::exec({ "adb", "start-server" });
        co_return res.transform(sink_void);
    }

    AExpect<Vec<Device>> list_devices()
    {
        auto res = co_await cmd::exec({ "adb", "devices" });

        if (not res.has_value()) {
            co_return Unexpect{ res.error() };
        }

        auto devices = Vec<Device>{};

        auto line_splitter = util::StringSplitter{ *res, { '\n' } };
        std::ignore        = line_splitter.next();    // skip the first line

        while (auto str = line_splitter.next()) {
            auto splitter = util::StringSplitter{ *str, { " \t" } };

            auto serial_str = splitter.next();
            auto status_str = splitter.next();

            if (not serial_str.has_value() or not status_str.has_value()) {
                continue;
            }

            auto status = DeviceStatus::Unknown;

            // clang-format off
            if      (*status_str == "offline")      status = DeviceStatus::Offline;
            else if (*status_str == "unauthorized") status = DeviceStatus::Unauthorized;
            else if (*status_str == "emulator")     status = DeviceStatus::Emulator;
            else if (*status_str == "device")       status = DeviceStatus::Device;
            // clang-format on

            devices.emplace_back(String{ *serial_str }, status);
        }

        co_return devices;
    }

    AExpect<Abi> get_abi(Str serial)
    {
        auto out = co_await cmd::exec({ "adb", "-s", serial, "shell", "getprop", "ro.product.cpu.abi" });
        if (not out) {
            co_return Unexpect{ out.error() };
        }
        auto str = util::strip(*out);

        // clang-format off
        if (str == "armeabi-v7a") co_return Abi::Armeabi_v7a;
        if (str == "arm64-v8a")   co_return Abi::Arm64_v8a;
        if (str == "x86")         co_return Abi::X86;
        if (str == "x86_64")      co_return Abi::X86_64;
        // clang-format on

        co_return Unexpect{ Errc::not_supported };
    }
}
