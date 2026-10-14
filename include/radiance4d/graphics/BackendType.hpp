// BackendType.hpp
#pragma once

#include <string>
#include <string_view>

namespace r4d
{

    enum class BackendType
    {
        D3D12,
        Vulkan,
    };

    constexpr std::string_view ToString(BackendType backend)
    {
        switch (backend)
        {
            case BackendType::D3D12:  return "D3D12";
            case BackendType::Vulkan: return "Vulkan";
        }
        return "Unknown";
    }

    bool parseBackendType(std::string_view name, BackendType& outBackend);

} // namespace r4d
