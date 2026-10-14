// GpuCapabilities.cpp
#include <radiance4d/graphics/GpuCapabilities.hpp>

#include <format>

namespace r4d
{

    std::string GpuCapabilities::ToString() const
    {
        return std::format(
            "feature level {}, shader model {}, enhanced barriers {}, tearing {}, "
            "binding tier {}, heap tier {}, raytracing {}, mesh shaders {}, runtime {}",
            featureLevel,
            highestShaderModel,
            enhancedBarriersSupported ? "yes" : "NO",
            tearingSupported ? "yes" : "no",
            resourceBindingTier,
            resourceHeapTier,
            rayTracingSupported ? "yes" : "no",
            meshShadersSupported ? "yes" : "no",
            runtimeDescription.empty() ? "unknown" : runtimeDescription);
    }

} // namespace r4d
