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

    // Highest spherical-harmonic band carried per splat. Degree 3 is what the
    // published 3DGS datasets store, at the cost of 48 of the 60 floats below.
    // R4D_SH_COEFFICIENTS in Common.hlsli must agree; the size assertion below is
    // the tripwire if one side moves without the other.
    inline constexpr int kShDegree = 3;
    inline constexpr int kShCoefficients = (kShDegree + 1) * (kShDegree + 1);

    // The degree-0 band is an offset from mid grey rather than a colour, so a
    // constant colour is kShC0 * sh[0..2] + 0.5. The shader uses the same
    // constant.
    inline constexpr float kShC0 = 0.28209479177387814f;

    // One 3D Gaussian, in the layout the GPU reads.
    //
    // Scales are linear and rotation is a normalised quaternion, stored xyzw as
    // glm lays it out; .ply datasets keep log-scales and unnormalised
    // quaternions, and loadGaussianPly converts.
    struct Gaussian
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float opacity = 1.0f;
        Vec3 scale{1.0f, 1.0f, 1.0f};
        float padding0 = 0.0f;
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        // Coefficient-major: band k, channel c at sh[k * 3 + c]. .ply files are
        // channel-major instead, and the loader transposes.
        float sh[kShCoefficients * 3]{};
    };

    static_assert(sizeof(Gaussian) == 240, "the GPU layout assumes 240 bytes");

    // A flat colour is a degree-0 cloud: the higher bands stay zero.
    inline void setConstantColour(Gaussian& g, const Vec3& colour)
    {
        g.sh[0] = (colour.x - 0.5f) / kShC0;
        g.sh[1] = (colour.y - 0.5f) / kShC0;
        g.sh[2] = (colour.z - 0.5f) / kShC0;
    }

    inline Vec3 constantColour(const Gaussian& g)
    {
        return Vec3{g.sh[0], g.sh[1], g.sh[2]} * kShC0 + 0.5f;
    }

    struct GaussianCloud
    {
        std::vector<Gaussian> gaussians;
        Bounds bounds;

        bool empty() const { return gaussians.empty(); }
        std::size_t count() const { return gaussians.size(); }

        // Padded by each splat's largest scale, so the bounds cover the splat
        // rather than just its centre.
        void recomputeBounds();
    };

    // Deterministic cloud for development and tests, so no multi-hundred-megabyte
    // dataset is needed to exercise the path.
    GaussianCloud makeProceduralGaussianCloud(std::uint32_t count = 20000,
                                              std::uint32_t seed = 1337);

    // Binary little-endian .ply as written by the 3DGS reference implementation.
    // Properties are matched by name, since exporters disagree on field order.
    //
    // No coordinate-system conversion is applied: positions, rotations and the SH
    // basis stay in the frame the file was authored in.
    Result<GaussianCloud> loadGaussianPly(const std::filesystem::path& path);

} // namespace r4d
