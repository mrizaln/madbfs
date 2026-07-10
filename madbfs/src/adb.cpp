#include "madbfs/adb.hpp"

#include "madbfs/cmd.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/split.hpp>

#if not defined(MADBFS_SERVER_BINARY_DIR)
#error "You must define MADBFS_SERVER_BINARY_BUILD_DIR so this file can embed the server binaries"
#endif

// clang-format off
#define MADBFS_SERVER_ARMEABI_V7A MADBFS_SERVER_BINARY_DIR/madbfs-server-armeabi-v7a
#define MADBFS_SERVER_ARM64_V8A   MADBFS_SERVER_BINARY_DIR/madbfs-server-arm64-v8a
#define MADBFS_SERVER_X86         MADBFS_SERVER_BINARY_DIR/madbfs-server-x86
#define MADBFS_SERVER_X86_64      MADBFS_SERVER_BINARY_DIR/madbfs-server-x86_64
// clang-format on

#define STRINGIFY_X(x) #x
#define STRINGIFY(x)   STRINGIFY_X(x)

constexpr std::uint8_t server_armeabi_v7a[] = {
#embed STRINGIFY(MADBFS_SERVER_ARMEABI_V7A)
};

constexpr std::uint8_t server_arm64_v8a[] = {
#embed STRINGIFY(MADBFS_SERVER_ARM64_V8A)
};

constexpr std::uint8_t server_x86[] = {
#embed STRINGIFY(MADBFS_SERVER_X86)
};

constexpr std::uint8_t server_x86_64[] = {
#embed STRINGIFY(MADBFS_SERVER_X86_64)
};

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

    Span<const u8> get_server(Abi abi)
    {
        switch (abi) {
        case Abi::Armeabi_v7a: return server_armeabi_v7a;
        case Abi::Arm64_v8a: return server_arm64_v8a;
        case Abi::X86: return server_x86;
        case Abi::X86_64: return server_x86_64;
        default: [[unlikely]] log_e(__func__, "Unknown ABI"); std::terminate();
        }
    }
}
