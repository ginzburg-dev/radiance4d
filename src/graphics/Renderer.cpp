// Renderer.cpp -- backend factory.
#include <radiance4d/graphics/Renderer.hpp>

#include <radiance4d/core/Log.hpp>

namespace r4d
{

#if R4D_ENABLE_D3D12
    namespace d3d12
    {
        Result<std::unique_ptr<Renderer>> createRenderer(const RendererDesc& desc);
    }
#endif

#if R4D_ENABLE_VULKAN
    namespace vulkan
    {
        Result<std::unique_ptr<Renderer>> createRenderer(const RendererDesc& desc);
    }
#endif

    Result<std::unique_ptr<Renderer>> createRenderer(BackendType backend,
                                                     const RendererDesc& desc)
    {
        R4D_INFO("Renderer: requested backend {}", ToString(backend));

        switch (backend)
        {
            case BackendType::D3D12:
#if R4D_ENABLE_D3D12
                return d3d12::createRenderer(desc);
#else
                return makeError(ErrorCode::UnsupportedFeature,
                                 "the D3D12 backend was disabled at build time "
                                 "(R4D_ENABLE_D3D12=OFF)");
#endif

            case BackendType::Vulkan:
#if R4D_ENABLE_VULKAN
                return vulkan::createRenderer(desc);
#else
                // A hard failure, not a fallback.
                return makeError(ErrorCode::UnsupportedFeature,
                                 "the Vulkan backend was disabled at build time "
                                 "(R4D_ENABLE_VULKAN=OFF)");
#endif
        }

        return makeError(ErrorCode::InvalidArgument, "unknown backend type");
    }

} // namespace r4d
