// GaussianCloud.hpp
#pragma once

#include <radiance4d/core/Math.hpp>
#include <radiance4d/core/Result.hpp>
#include <radiance4d/scene/Model.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace r4d
{

    // One 3D Gaussian, in the layout the GPU reads.
    //
    // Scales are linear and rotation is a normalised quaternion, stored xyzw as
    // glm lays it out; .ply datasets keep log-scales and unnormalised
    // quaternions, and the loader converts.
    // Only the degree-0 spherical harmonic is kept for now, which is the constant
    // colour term; view-dependent bands come with the full SH evaluation.
    struct Gaussian
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float opacity = 1.0f;
        Vec3 scale{1.0f, 1.0f, 1.0f};
        float padding0 = 0.0f;
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 colour{1.0f, 1.0f, 1.0f};
        float padding1 = 0.0f;
    };

    static_assert(sizeof(Gaussian) == 64, "the GPU layout assumes 64 bytes");

    struct GaussianCloud
    {
        std::vector<Gaussian> gaussians;
        Bounds bounds;

        bool empty() const { return gaussians.empty(); }
        std::size_t count() const { return gaussians.size(); }
    };

    // Deterministic cloud for development and tests, so no multi-hundred-megabyte
    // dataset is needed to exercise the path.
    GaussianCloud makeProceduralGaussianCloud(std::uint32_t count = 20000,
                                              std::uint32_t seed = 1337);

} // namespace r4d
