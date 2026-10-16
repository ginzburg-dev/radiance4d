// GpuStats.hpp
#pragma once

#include <radiance4d/core/Math.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace r4d
{

    // The splat path's stages, coarse enough to stay readable: the depth sort's
    // eight radix passes and the tile sort's two collapse into one entry each,
    // rather than timing all sixteen dispatches separately.
    enum class GpuPass : std::size_t
    {
        SplatCull,
        SplatDepthSort,
        SplatBinning,
        SplatTileSort,
        SplatRaster,
        SplatComposite,
        Count
    };

    constexpr std::size_t kGpuPassCount = static_cast<std::size_t>(GpuPass::Count);

    constexpr const char* gpuPassName(GpuPass pass)
    {
        switch (pass)
        {
        case GpuPass::SplatCull:
            return "Cull";
        case GpuPass::SplatDepthSort:
            return "Depth sort";
        case GpuPass::SplatBinning:
            return "Binning";
        case GpuPass::SplatTileSort:
            return "Tile sort";
        case GpuPass::SplatRaster:
            return "Rasterise";
        case GpuPass::SplatComposite:
            return "Composite";
        case GpuPass::Count:
            break;
        }
        return "unknown";
    }

    struct GpuStats
    {
        double cpuFrameMs = 0.0;
        double cpuFrameAverageMs = 0.0;

        // Ignore the GPU fields unless gpuTimingsValid; they are unset, not zero,
        // until the first timestamp resolve lands.
        double gpuFrameMs = 0.0;
        double gpuFrameAverageMs = 0.0;
        bool gpuTimingsValid = false;

        // Indexed by GpuPass. Only meaningful while gpuPassTimingsValid, which
        // needs the splat path to be running and the scopes to be enabled.
        std::array<double, kGpuPassCount> gpuPassMs{};
        std::array<double, kGpuPassCount> gpuPassAverageMs{};
        bool gpuPassTimingsValid = false;

        std::uint32_t drawCalls = 0;
        std::uint32_t dispatches = 0;
        std::uint32_t triangles = 0;
        UVec2 renderResolution{0, 0};
        std::uint64_t frameIndex = 0;
    };

} // namespace r4d
