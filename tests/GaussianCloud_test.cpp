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
    // The shader reads this as a 64-byte struct; padding is part of the contract.
    EXPECT_EQ(sizeof(r4d::Gaussian), 64u);
    EXPECT_EQ(offsetof(r4d::Gaussian, opacity), 12u);
    EXPECT_EQ(offsetof(r4d::Gaussian, scale), 16u);
    EXPECT_EQ(offsetof(r4d::Gaussian, rotation), 32u);
    EXPECT_EQ(offsetof(r4d::Gaussian, colour), 48u);
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
