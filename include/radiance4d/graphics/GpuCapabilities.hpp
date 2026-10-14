// GpuCapabilities.hpp
#pragma once

#include <cstdint>
#include <string>

namespace r4d
{

    // Named GpuCapabilities, not DeviceCapabilities: the latter is a wingdi.h
    // macro that expands to DeviceCapabilitiesW wherever windows.h is included.
    struct GpuCapabilities
    {
        std::string featureLevel;
        std::string highestShaderModel;
        bool enhancedBarriersSupported = false;
        bool tearingSupported = false;
        std::uint32_t resourceBindingTier = 0;
        std::uint32_t resourceHeapTier = 0;
        bool rayTracingSupported = false;
        bool meshShadersSupported = false;

        // Which d3d12 runtime actually serviced the device.
        std::string runtimeDescription;
        std::uint32_t agilitySdkVersion = 0;

        std::string ToString() const;
    };

} // namespace r4d
