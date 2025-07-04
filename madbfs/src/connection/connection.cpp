#include "madbfs/connection/connection.hpp"

#include "madbfs/cmd.hpp"

#include <madbfs-common/util/split.hpp>

namespace madbfs::connection
{
    using namespace std::string_view_literals;

    Str to_string(DeviceStatus status)
    {
        switch (status) {
        case DeviceStatus::Device: return "device ok";
        case DeviceStatus::Offline: return "device offline";
        case DeviceStatus::Unauthorized: return "device unauthorized";
        case DeviceStatus::Unknown: return "unknown";
        }
        return "Unknown";
    }

    AExpect<void> start_connection()
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
            else if (*status_str == "device")       status = DeviceStatus::Device;
            // clang-format on

            devices.emplace_back(String{ *serial_str }, status);
        }

        co_return devices;
    }
}
