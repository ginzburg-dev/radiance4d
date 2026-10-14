// GpuStats.hpp
#pragma once

#include <radiance4d/core/Math.hpp>

#include <cstdint>

namespace r4d
{

    struct GpuStats
    {
        double cpuFrameMs = 0.0;
        double cpuFrameAverageMs = 0.0;

        // Ignore the GPU fields unless gpuTimingsValid; they are unset, not zero,
        // until the first timestamp resolve lands.
        double gpuFrameMs = 0.0;
        double gpuFrameAverageMs = 0.0;
        bool gpuTimingsValid = false;

        std::uint32_t drawCalls = 0;
        std::uint32_t dispatches = 0;
        std::uint32_t triangles = 0;
        UVec2 renderResolution{0, 0};
        std::uint64_t frameIndex = 0;
    };

} // namespace r4d
