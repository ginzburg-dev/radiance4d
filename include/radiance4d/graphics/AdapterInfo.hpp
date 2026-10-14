// AdapterInfo.hpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace r4d
{

    struct AdapterInfo
    {
        std::string name;
        std::uint32_t vendorId = 0;
        std::uint32_t deviceId = 0;
        std::uint64_t dedicatedVideoMemory = 0;
        std::uint64_t dedicatedSystemMemory = 0;
        std::uint64_t sharedSystemMemory = 0;
        bool isSoftware = false;

        std::string ToString() const;
    };

    std::string_view vendorName(std::uint32_t vendorId);

} // namespace r4d
