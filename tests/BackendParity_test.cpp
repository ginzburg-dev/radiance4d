// BackendParity_test.cpp
//
// Renders the same scene through both backends on the same GPU and compares the
// results. Skipped when Vulkan is unavailable, and labelled "gpu" because there
// is no software Vulkan device the way WARP covers D3D12.

#include <radiance4d/core/Log.hpp>
#include <radiance4d/graphics/Renderer.hpp>
#include <radiance4d/scene/Camera.hpp>
#include <radiance4d/scene/Model.hpp>

#include "ImageCompare.hpp"

#include <gtest/gtest.h>

namespace
{
    constexpr std::uint32_t kWidth = 640;
    constexpr std::uint32_t kHeight = 360;
    constexpr double kFixedStep = 1.0 / 60.0;
    constexpr int kFrames = 8;

    // Measured on an RTX 3080 Laptop GPU: max channel delta 1, RMSE 0.0018. The
    // allowance is wider so a driver revision does not fail the build, but far
    // too tight to hide a real divergence in conventions or shading.
    constexpr std::uint32_t kPerPixelTolerance = 2;
    constexpr std::uint32_t kMaxChannelDelta = 4;
    constexpr double kMaxRmse = 0.5;
    constexpr double kMaxDifferingFraction = 0.001;

    r4d::Result<r4d::FrameCapture> renderWith(r4d::BackendType backend)
    {
        r4d::RendererDesc desc;
        desc.windowHandle = nullptr;
        desc.initialSize = r4d::UVec2{kWidth, kHeight};
        desc.enableDebugLayer = true;
        desc.fixedTimeStep = kFixedStep;

        auto renderer = r4d::createRenderer(backend, desc);
        if (!renderer)
        {
            return renderer.error();
        }
        auto& gpu = *renderer.value();

        const r4d::Model model = r4d::makeDemoScene(3, 2, 16);
        if (auto status = gpu.uploadModel(model); !status)
        {
            return status.error();
        }

        r4d::Camera camera;
        camera.setAspect(static_cast<float>(kWidth) / static_cast<float>(kHeight));
        camera.focusOn(model.bounds.centre(), model.bounds.radius());
        gpu.setCamera(r4d::CameraParams{camera.view(), camera.projection(),
                                        camera.position()});

        for (int i = 0; i < kFrames; ++i)
        {
            if (auto status = gpu.renderFrame(); !status)
            {
                return status.error();
            }
        }

        if (gpu.validationErrorCount() != 0)
        {
            return r4d::makeError(r4d::ErrorCode::ApiCallFailed,
                                  "validation errors were reported while rendering");
        }
        return gpu.captureFrame();
    }

    class BackendParity : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            r4d::Logger::instance().init(r4d::LogLevel::Warning, true, {});
            r4d::Logger::instance().resetCounters();
        }
    };
} // namespace

TEST_F(BackendParity, VulkanMatchesD3D12WithinTolerance)
{
    auto vulkan = renderWith(r4d::BackendType::Vulkan);
    if (!vulkan && vulkan.error().code == r4d::ErrorCode::UnsupportedFeature)
    {
        GTEST_SKIP() << "Vulkan backend unavailable: " << vulkan.error().message;
    }
    ASSERT_TRUE(vulkan.ok()) << vulkan.error().ToString();

    auto d3d12 = renderWith(r4d::BackendType::D3D12);
    ASSERT_TRUE(d3d12.ok()) << d3d12.error().ToString();

    const auto difference = r4d::testing::compareImages(vulkan.value(), d3d12.value(),
                                                        kPerPixelTolerance);
    ASSERT_TRUE(difference.comparable) << difference.message;

    if (difference.maxChannelDelta > kMaxChannelDelta || difference.rmse > kMaxRmse ||
        difference.differingFraction() > kMaxDifferingFraction)
    {
        r4d::testing::writeFailureArtefacts(
            std::filesystem::path{R4D_TEST_DATA_DIR} / "backend_parity.png", vulkan.value(),
            d3d12.value());
        FAIL() << "the backends diverged: " << difference.message;
    }

    SUCCEED() << difference.message;
}

TEST_F(BackendParity, BothBackendsReportTheSameResolutionAndDrawCount)
{
    // Guards against a parity pass that only happens because one backend drew
    // nothing: an empty frame would also match an empty frame.
    r4d::RendererDesc desc;
    desc.windowHandle = nullptr;
    desc.initialSize = r4d::UVec2{kWidth, kHeight};
    desc.fixedTimeStep = kFixedStep;

    auto vulkan = r4d::createRenderer(r4d::BackendType::Vulkan, desc);
    if (!vulkan && vulkan.error().code == r4d::ErrorCode::UnsupportedFeature)
    {
        GTEST_SKIP() << "Vulkan backend unavailable";
    }
    ASSERT_TRUE(vulkan.ok()) << vulkan.error().ToString();

    auto d3d12 = r4d::createRenderer(r4d::BackendType::D3D12, desc);
    ASSERT_TRUE(d3d12.ok()) << d3d12.error().ToString();

    const r4d::Model model = r4d::makeDemoScene(3, 2, 16);
    ASSERT_TRUE(vulkan.value()->uploadModel(model).ok());
    ASSERT_TRUE(d3d12.value()->uploadModel(model).ok());
    ASSERT_TRUE(vulkan.value()->renderFrame().ok());
    ASSERT_TRUE(d3d12.value()->renderFrame().ok());

    EXPECT_EQ(vulkan.value()->stats().drawCalls, d3d12.value()->stats().drawCalls);
    EXPECT_EQ(vulkan.value()->stats().triangles, d3d12.value()->stats().triangles);
    EXPECT_EQ(vulkan.value()->stats().renderResolution,
              d3d12.value()->stats().renderResolution);
    EXPECT_GT(vulkan.value()->stats().drawCalls, 0u);

    EXPECT_EQ(vulkan.value()->backend(), r4d::BackendType::Vulkan);
    EXPECT_EQ(d3d12.value()->backend(), r4d::BackendType::D3D12);
}
