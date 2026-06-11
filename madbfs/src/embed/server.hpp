#pragma once

#include "madbfs/adb.hpp"

#include <cstdint>
#include <span>

namespace madbfs::embed
{
    /**
     * @brief Get madbfs server for specified abi
     *
     * @param abi Phone ABI.
     *
     * @return Server binary data.
     */
    std::span<const std::uint8_t> get_server(madbfs::adb::Abi abi);
}
