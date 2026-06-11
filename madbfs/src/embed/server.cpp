#include "./server.hpp"

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

namespace madbfs::embed
{
    std::span<const std::uint8_t> get_server(madbfs::adb::Abi abi)
    {
        switch (abi) {
        case madbfs::adb::Abi::Armeabi_v7a: return server_armeabi_v7a;
        case madbfs::adb::Abi::Arm64_v8a: return server_arm64_v8a;
        case madbfs::adb::Abi::X86: return server_x86;
        case madbfs::adb::Abi::X86_64: return server_x86_64;
        default: [[unlikely]] std::terminate();
        }
    }
}
