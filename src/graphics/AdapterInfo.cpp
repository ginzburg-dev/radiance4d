// AdapterInfo.cpp
#include <radiance4d/graphics/AdapterInfo.hpp>

#include <format>

namespace r4d
{

    std::string_view vendorName(std::uint32_t vendorId)
    {
        switch (vendorId)
        {
            case 0x10DE: return "NVIDIA";
            case 0x1002: return "AMD";
            case 0x8086: return "Intel";
            case 0x1414: return "Microsoft";   // WARP / Basic Render Driver
            case 0x13B5: return "ARM";
            case 0x5143: return "Qualcomm";
            default:     return "Unknown vendor";
        }
    }

    std::string AdapterInfo::ToString() const
    {
        const double vramGiB =
            static_cast<double>(dedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0);
        return std::format(
            "{} [{} 0x{:04X}:0x{:04X}] VRAM {:.2f} GiB, shared {:.2f} GiB{}",
            name, vendorName(vendorId), vendorId, deviceId, vramGiB,
            static_cast<double>(sharedSystemMemory) / (1024.0 * 1024.0 * 1024.0),
            isSoftware ? " (SOFTWARE)" : "");
    }

} // namespace r4d
