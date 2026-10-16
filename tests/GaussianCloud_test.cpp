// GaussianCloud_test.cpp
//
// The procedural cloud backs the splat image reference, so its determinism is a
// correctness requirement, not a convenience.

#include <radiance4d/scene/GaussianCloud.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    bool sameBytes(const r4d::Gaussian& a, const r4d::Gaussian& b)
    {
        return std::memcmp(&a, &b, sizeof(r4d::Gaussian)) == 0;
    }
} // namespace

TEST(GaussianCloud, MatchesTheGpuLayout)
{
    // The shader reads this as a 240-byte struct; padding is part of the
    // contract, and spirv-dis reports the same offsets and an ArrayStride of 240.
    EXPECT_EQ(sizeof(r4d::Gaussian), 240u);
    EXPECT_EQ(offsetof(r4d::Gaussian, opacity), 12u);
    EXPECT_EQ(offsetof(r4d::Gaussian, scale), 16u);
    EXPECT_EQ(offsetof(r4d::Gaussian, rotation), 32u);
    EXPECT_EQ(offsetof(r4d::Gaussian, sh), 48u);

    // R4D_SH_COEFFICIENTS in Common.hlsli sizes the shader's copy.
    EXPECT_EQ(r4d::kShCoefficients, 16);
}

TEST(GaussianCloud, AFlatColourRoundTripsThroughTheDegreeZeroBand)
{
    // The procedural cloud sets a colour and the shader reconstructs it from the
    // SH, so the two conventions have to agree.
    r4d::Gaussian g;
    const r4d::Vec3 colour{0.25f, 0.5f, 0.95f};
    r4d::setConstantColour(g, colour);

    const r4d::Vec3 recovered = r4d::constantColour(g);
    EXPECT_NEAR(recovered.x, colour.x, 1e-5f);
    EXPECT_NEAR(recovered.y, colour.y, 1e-5f);
    EXPECT_NEAR(recovered.z, colour.z, 1e-5f);

    for (int i = 3; i < r4d::kShCoefficients * 3; ++i)
    {
        EXPECT_EQ(g.sh[i], 0.0f) << "slot " << i;
    }
}

TEST(GaussianCloud, SameSeedGivesTheSameCloud)
{
    const auto first = r4d::makeProceduralGaussianCloud(512, 7);
    const auto second = r4d::makeProceduralGaussianCloud(512, 7);

    ASSERT_EQ(first.count(), 512u);
    ASSERT_EQ(first.count(), second.count());
    for (std::size_t i = 0; i < first.count(); ++i)
    {
        EXPECT_TRUE(sameBytes(first.gaussians[i], second.gaussians[i])) << "splat " << i;
    }
}

TEST(GaussianCloud, DifferentSeedsDiverge)
{
    const auto first = r4d::makeProceduralGaussianCloud(512, 7);
    const auto second = r4d::makeProceduralGaussianCloud(512, 8);

    std::size_t identical = 0;
    for (std::size_t i = 0; i < first.count(); ++i)
    {
        if (sameBytes(first.gaussians[i], second.gaussians[i]))
        {
            ++identical;
        }
    }
    // The ribbon family is a fixed curve, so some splats legitimately repeat.
    EXPECT_LT(identical, first.count() / 2);
}

TEST(GaussianCloud, BoundsCoverEverySplat)
{
    const auto cloud = r4d::makeProceduralGaussianCloud(4096, 1337);
    ASSERT_TRUE(cloud.bounds.valid());

    for (const r4d::Gaussian& g : cloud.gaussians)
    {
        EXPECT_GE(g.position.x, cloud.bounds.min.x);
        EXPECT_GE(g.position.y, cloud.bounds.min.y);
        EXPECT_GE(g.position.z, cloud.bounds.min.z);
        EXPECT_LE(g.position.x, cloud.bounds.max.x);
        EXPECT_LE(g.position.y, cloud.bounds.max.y);
        EXPECT_LE(g.position.z, cloud.bounds.max.z);
    }
}

TEST(GaussianCloud, RotationsAreUnitQuaternions)
{
    // The shader builds a rotation matrix from these without renormalising.
    const auto cloud = r4d::makeProceduralGaussianCloud(2048, 99);
    for (const r4d::Gaussian& g : cloud.gaussians)
    {
        EXPECT_NEAR(glm::length(g.rotation), 1.0f, 1e-4f);
        EXPECT_GT(g.opacity, 0.0f);
        EXPECT_LE(g.opacity, 1.0f);
        EXPECT_GT(g.scale.x, 0.0f);
        EXPECT_GT(g.scale.y, 0.0f);
        EXPECT_GT(g.scale.z, 0.0f);
    }
}

TEST(GaussianCloud, EmptyCloudIsAllowed)
{
    const auto cloud = r4d::makeProceduralGaussianCloud(0, 1);
    EXPECT_TRUE(cloud.empty());
    EXPECT_EQ(cloud.count(), 0u);
}

namespace
{
    // A compact subject plus the sparse far tail a trained reconstruction
    // leaves behind: 1000 splats filling a 2-unit cube, and 50 strays marching
    // out to 1000 units along +x.
    r4d::GaussianCloud makeCloudWithAnOutlierTail()
    {
        const r4d::Vec3 centre{5.0f, -2.0f, 3.0f};

        r4d::GaussianCloud cloud;
        for (int i = 0; i < 1000; ++i)
        {
            r4d::Gaussian g;
            g.position =
                centre +
                r4d::Vec3{static_cast<float>(i % 10), static_cast<float>((i / 10) % 10),
                          static_cast<float>((i / 100) % 10)} *
                    (2.0f / 9.0f) -
                r4d::Vec3{1.0f};
            g.scale = r4d::Vec3{0.01f};
            cloud.gaussians.push_back(g);
        }
        for (int i = 1; i <= 50; ++i)
        {
            r4d::Gaussian g;
            g.position = centre + r4d::Vec3{static_cast<float>(i) * 20.0f, 0.0f, 0.0f};
            g.scale = r4d::Vec3{0.01f};
            cloud.gaussians.push_back(g);
        }

        cloud.recomputeBounds();
        return cloud;
    }

    std::size_t countInside(const r4d::GaussianCloud& cloud, const r4d::Sphere& sphere)
    {
        std::size_t inside = 0;
        for (const r4d::Gaussian& g : cloud.gaussians)
        {
            if (glm::length(g.position - sphere.centre) <= sphere.radius)
            {
                ++inside;
            }
        }
        return inside;
    }
} // namespace

TEST(GaussianCloud, FramingIgnoresAFarOutlierTail)
{
    // The whole point: the bounds have to cover the strays, and the camera must
    // not. Without this the subject is a few percent of the frame.
    const auto cloud = makeCloudWithAnOutlierTail();

    ASSERT_TRUE(cloud.bounds.valid());
    EXPECT_GT(cloud.bounds.radius(), 400.0f);

    // On the scale of the subject, and nothing like the tail.
    EXPECT_GE(cloud.framing.radius, 0.5f);
    EXPECT_LT(cloud.framing.radius, 2.0f);

    EXPECT_NEAR(cloud.framing.centre.x, 5.0f, 0.3f);
    EXPECT_NEAR(cloud.framing.centre.y, -2.0f, 0.3f);
    EXPECT_NEAR(cloud.framing.centre.z, 3.0f, 0.3f);

    // The default quantile is a half, so about half the splats are inside by
    // construction -- and every one of them is a subject splat, not a stray.
    const std::size_t inside = countInside(cloud, cloud.framing);
    EXPECT_GE(inside, 500u);
    EXPECT_LE(inside, 700u);
}

TEST(GaussianCloud, FramingIsNeverWiderThanTheCloud)
{
    // A well-behaved cloud has no tail to cut, so framing must not zoom out past
    // the bounds it would have used before.
    const auto compact = r4d::makeProceduralGaussianCloud(4096, 1337);
    EXPECT_LE(compact.framing.radius, compact.bounds.radius());

    // Nor should it collapse onto the centre: on a cloud with no tail the
    // median distance is a large fraction of the radius (0.794 of it for a
    // uniformly filled ball), so the framing stays close to the bounds.
    EXPECT_GT(compact.framing.radius, compact.bounds.radius() * 0.3f);

    const auto tailed = makeCloudWithAnOutlierTail();
    EXPECT_LE(tailed.framing.radius, tailed.bounds.radius());
}

TEST(GaussianCloud, FramingGrowsWithTheQuantileAndStopsAtTheFurthestSplat)
{
    const auto cloud = makeCloudWithAnOutlierTail();
    const auto& splats = cloud.gaussians;

    const r4d::Sphere tight = r4d::robustFramingSphere(splats, 0.0f);
    const r4d::Sphere usual = r4d::robustFramingSphere(splats, r4d::kFramingQuantile);
    const r4d::Sphere wide = r4d::robustFramingSphere(splats, 1.0f);

    EXPECT_LT(tight.radius, usual.radius);
    EXPECT_LT(usual.radius, wide.radius);

    // Quantile 1 excludes nothing, so it is the whole cloud, padded.
    float furthest = 0.0f;
    for (const r4d::Gaussian& g : splats)
    {
        furthest = std::max(furthest, glm::length(g.position - wide.centre));
    }
    EXPECT_NEAR(wide.radius, furthest + 0.03f, 1e-3f);

    // And it is clamped, not extrapolated, past the ends.
    EXPECT_EQ(r4d::robustFramingSphere(splats, 7.0f).radius, wide.radius);
    EXPECT_EQ(r4d::robustFramingSphere(splats, -1.0f).radius, tight.radius);
}

TEST(GaussianCloud, FramingHandlesEmptyAndSingleSplatClouds)
{
    const r4d::Sphere none = r4d::robustFramingSphere({});
    EXPECT_EQ(none.radius, 0.0f);
    EXPECT_EQ(none.centre, r4d::Vec3{0.0f});

    r4d::Gaussian only;
    only.position = r4d::Vec3{1.0f, 2.0f, 3.0f};
    only.scale = r4d::Vec3{0.25f, 0.5f, 0.125f};
    const r4d::Sphere one = r4d::robustFramingSphere({only});
    EXPECT_EQ(one.centre, only.position);
    // Zero spread, so all that is left is the padding: the splat's own extent.
    EXPECT_NEAR(one.radius, 1.5f, 1e-5f);
}

TEST(Sphere, MergeCoversBothAndKeepsTheOuterOne)
{
    const r4d::Sphere small{r4d::Vec3{1.0f, 0.0f, 0.0f}, 1.0f};
    const r4d::Sphere large{r4d::Vec3{0.0f, 0.0f, 0.0f}, 5.0f};

    // Nested: the outer sphere already covers it, so nothing grows.
    const r4d::Sphere nested = merge(large, small);
    EXPECT_EQ(nested.radius, large.radius);
    EXPECT_EQ(nested.centre, large.centre);
    EXPECT_EQ(merge(small, large).radius, large.radius);

    // Disjoint: 10 apart with radii 1 and 2 spans -1..12, so radius 6.5 at 5.5.
    const r4d::Sphere apart = merge(r4d::Sphere{r4d::Vec3{0.0f}, 1.0f},
                                    r4d::Sphere{r4d::Vec3{10.0f, 0.0f, 0.0f}, 2.0f});
    EXPECT_NEAR(apart.radius, 6.5f, 1e-5f);
    EXPECT_NEAR(apart.centre.x, 5.5f, 1e-5f);

    // A box's sphere is its half-diagonal, not its half-extent.
    const r4d::Sphere box =
        r4d::boundingSphere(r4d::Bounds{r4d::Vec3{-1.0f}, r4d::Vec3{1.0f}});
    EXPECT_NEAR(box.radius, std::sqrt(3.0f), 1e-5f);
}
