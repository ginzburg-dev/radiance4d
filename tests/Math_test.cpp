// Math_test.cpp
//
// Pins down the GLM configuration rather than re-testing GLM.

#include <radiance4d/core/Math.hpp>

#include <gtest/gtest.h>

TEST(Math, DepthRangeIsZeroToOne)
{
    // D3D12 and Vulkan want depth in [0,1]; GLM defaults to OpenGL range.
#if !defined(GLM_FORCE_DEPTH_ZERO_TO_ONE)
    FAIL() << "GLM_FORCE_DEPTH_ZERO_TO_ONE is not defined; clip-space depth would be "
              "OpenGL's [-1,1] and nothing would draw correctly under D3D12";
#endif

    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 100.0f;
    const r4d::Mat4 projection =
        glm::perspective(r4d::degreesToRadians(60.0f), 16.0f / 9.0f, nearPlane, farPlane);

    // Near plane maps to 0, far plane to 1.
    const r4d::Vec4 atNear = projection * r4d::Vec4{0.0f, 0.0f, nearPlane, 1.0f};
    const r4d::Vec4 atFar = projection * r4d::Vec4{0.0f, 0.0f, farPlane, 1.0f};

    ASSERT_GT(atNear.w, 0.0f);
    ASSERT_GT(atFar.w, 0.0f);
    EXPECT_NEAR(atNear.z / atNear.w, 0.0f, 1e-4f);
    EXPECT_NEAR(atFar.z / atFar.w, 1.0f, 1e-4f);
}

TEST(Math, CoordinateSystemIsLeftHanded)
{
#if !defined(GLM_FORCE_LEFT_HANDED)
    FAIL() << "GLM_FORCE_LEFT_HANDED is not defined; the engine assumes a left-handed "
              "world space";
#endif
    // Left-handed: x cross y gives +z.
    const r4d::Vec3 crossed =
        glm::cross(r4d::Vec3{1.0f, 0.0f, 0.0f}, r4d::Vec3{0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(crossed.z, 1.0f, 1e-6f);
}

TEST(Math, MatricesAreColumnMajorInMemory)
{
    // Uploading without a transpose requires contiguous columns.
    r4d::Mat4 m{1.0f};
    m[3] = r4d::Vec4{7.0f, 8.0f, 9.0f, 1.0f};

    const float* raw = glm::value_ptr(m);
    EXPECT_FLOAT_EQ(raw[12], 7.0f);
    EXPECT_FLOAT_EQ(raw[13], 8.0f);
    EXPECT_FLOAT_EQ(raw[14], 9.0f);
    EXPECT_FLOAT_EQ(raw[15], 1.0f);
    EXPECT_EQ(sizeof(r4d::Mat4), 16u * sizeof(float));
}

TEST(Math, AngleConversionRoundTrips)
{
    EXPECT_NEAR(r4d::degreesToRadians(180.0f), r4d::kPi, 1e-6f);
    EXPECT_NEAR(r4d::radiansToDegrees(r4d::kPi), 180.0f, 1e-4f);
    EXPECT_NEAR(r4d::radiansToDegrees(r4d::degreesToRadians(37.5f)), 37.5f, 1e-4f);
}
