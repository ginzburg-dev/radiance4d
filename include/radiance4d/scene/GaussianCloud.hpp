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

    // Which quantile of the distance-to-centre distribution the framing sphere
    // takes as its radius. Half the splats is the sphere that holds the subject:
    //
    //  - On a uniformly filled ball the median distance is cbrt(1/2) = 0.794 of
    //    the radius, so a well-behaved cloud is framed much as its bounds would
    //    frame it, a little tighter.
    //  - On a reconstruction it ignores the tail outright. INRIA's `train` has a
    //    median distance of 2.39 against a bounds radius of 145, and its 301
    //    capture cameras stand a median 3.42 and at most 7.35 from the centre.
    //    Camera::focusOn turns radius 2.39 into a camera 5.4 back, inside the
    //    volume the scene was photographed from and so inside the volume it
    //    reconstructs; radius 145 puts it 326 back, where the subject is a few
    //    pixels wide.
    inline constexpr float kFramingQuantile = 0.5f;

    // Median centre, and the distance from it that `quantile` of the splats are
    // within. Padded like recomputeBounds, but by the median largest scale
    // rather than the maximum, so one huge splat cannot stretch it.
    //
    // This is for pointing a camera at the subject, not for culling: it
    // deliberately leaves splats outside itself.
    Sphere robustFramingSphere(const std::vector<Gaussian>& gaussians,
                               float quantile = kFramingQuantile);

    struct GaussianCloud
    {
        std::vector<Gaussian> gaussians;
        Bounds bounds;

        // Where to put a camera. A trained reconstruction sprays sparse
        // low-opacity splats far outside the subject, so for real datasets this
        // is very much smaller than the bounds: on INRIA's `train` scene, radius
        // 2.4 against the bounds' 145.
        Sphere framing;

        bool empty() const { return gaussians.empty(); }
        std::size_t count() const { return gaussians.size(); }

        // Fills both: bounds, padded by each splat's largest scale so they cover
        // the splat rather than just its centre, and the framing sphere.
        // Everything that needs the true extent — culling, the near and far
        // planes, the merge with a mesh's bounds — still reads bounds.
        void recomputeBounds();
    };

    // Deterministic cloud for development and tests, so no multi-hundred-megabyte
    // dataset is needed to exercise the path.
    GaussianCloud makeProceduralGaussianCloud(std::uint32_t count = 20000,
                                              std::uint32_t seed = 1337);

    // Binary little-endian .ply as written by the 3DGS reference implementation.
    // Properties are matched by name, since exporters disagree on field order.
    //
    // The file's frame is COLMAP's, right-handed with +y down, and the engine's
    // is left-handed with +y up, so the loader mirrors y: positions, the rotation
    // quaternion and the SH bands that are odd in y all change frame together.
    // Without it a published scene loads upside down.
    Result<GaussianCloud> loadGaussianPly(const std::filesystem::path& path);

} // namespace r4d
