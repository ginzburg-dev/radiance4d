// BackendType.cpp
#include <radiance4d/graphics/BackendType.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>

namespace r4d
{

    bool parseBackendType(std::string_view name, BackendType& outBackend)
    {
        std::string lowered;
        lowered.reserve(name.size());
        std::transform(name.begin(), name.end(), std::back_inserter(lowered),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowered == "d3d12" || lowered == "dx12" || lowered == "directx12")
        {
            outBackend = BackendType::D3D12;
            return true;
        }
        if (lowered == "vulkan" || lowered == "vk")
        {
            outBackend = BackendType::Vulkan;
            return true;
        }
        return false;
    }

} // namespace r4d
