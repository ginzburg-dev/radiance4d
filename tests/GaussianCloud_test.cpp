// GaussianCloud_test.cpp
//
// The procedural cloud backs the splat image reference, so its determinism is a
// correctness requirement, not a convenience.

#include <radiance4d/scene/GaussianCloud.hpp>

#include <gtest/gtest.h>

#include <cstring>

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
