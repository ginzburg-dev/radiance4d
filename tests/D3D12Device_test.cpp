// D3D12Device_test.cpp
//
// Runs on WARP so it works without a GPU, but still exercises the real D3D12
// path: same runtime, same barriers, same validation.

#include <radiance4d/core/Log.hpp>
#include <radiance4d/graphics/Renderer.hpp>
#include <radiance4d/scene/Camera.hpp>
#include <radiance4d/scene/Model.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace
{
    r4d::RendererDesc warpDesc(std::uint32_t width = 320, std::uint32_t height = 180)
    {
        r4d::RendererDesc desc;
        desc.windowHandle = nullptr;
        desc.initialSize = r4d::UVec2{width, height};
        desc.adapterSelector = "warp";
        desc.enableDebugLayer = true;
        desc.fixedTimeStep = 1.0 / 60.0;
        return desc;
    }

    class WarpDevice : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            r4d::Logger::instance().init(r4d::LogLevel::Warning, true, {});
            r4d::Logger::instance().resetCounters();
        }
    };
} // namespace

TEST_F(WarpDevice, CreatesHeadlessRendererOnTheSoftwareAdapter)
{
    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, warpDesc());
    ASSERT_TRUE(renderer.ok()) << renderer.error().ToString();

    auto& gpu = *renderer.value();
    EXPECT_EQ(gpu.backend(), r4d::BackendType::D3D12);
    EXPECT_TRUE(gpu.headless());
    EXPECT_TRUE(gpu.adapterInfo().isSoftware)
        << "expected the software adapter, got " << gpu.adapterInfo().name;
}

TEST_F(WarpDevice, ReportsEnhancedBarrierSupport)
{
    // Creation should fail outright if these are missing.
    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, warpDesc());
    ASSERT_TRUE(renderer.ok()) << renderer.error().ToString();

    const auto& caps = renderer.value()->capabilities();
    EXPECT_TRUE(caps.enhancedBarriersSupported);
    EXPECT_FALSE(caps.featureLevel.empty());
    EXPECT_FALSE(caps.highestShaderModel.empty());
    EXPECT_FALSE(caps.runtimeDescription.empty());
}

TEST_F(WarpDevice, RendersFramesWithoutValidationErrors)
{
    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, warpDesc());
    ASSERT_TRUE(renderer.ok()) << renderer.error().ToString();
    auto& gpu = *renderer.value();

    const r4d::Model model = r4d::makeDemoScene(2, 2, 12);
    ASSERT_TRUE(gpu.uploadModel(model).ok());

    // More than the frames in flight, so allocator reuse is exercised.
    constexpr int kFrames = 12;
    for (int i = 0; i < kFrames; ++i)
    {
        ASSERT_TRUE(gpu.renderFrame().ok()) << "frame " << i;
    }

    EXPECT_EQ(gpu.validationErrorCount(), 0u);
    EXPECT_EQ(gpu.stats().frameIndex, static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(gpu.stats().drawCalls, static_cast<std::uint32_t>(model.primitives.size()));
}

TEST_F(WarpDevice, ResizesWithoutValidationErrors)
{
    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, warpDesc());
    ASSERT_TRUE(renderer.ok()) << renderer.error().ToString();
    auto& gpu = *renderer.value();

    ASSERT_TRUE(gpu.renderFrame().ok());

    // Odd sizes catch assumptions about dimensions and row padding.
    for (const r4d::UVec2 size : {r4d::UVec2{201, 97}, r4d::UVec2{640, 360},
                                  r4d::UVec2{63, 129}})
    {
        ASSERT_TRUE(gpu.resize(size).ok()) << size.x << "x" << size.y;
        ASSERT_TRUE(gpu.renderFrame().ok()) << size.x << "x" << size.y;
        EXPECT_EQ(gpu.stats().renderResolution, size);
    }

    EXPECT_EQ(gpu.validationErrorCount(), 0u);
}

TEST_F(WarpDevice, RejectsAnUnknownAdapterInsteadOfFallingBack)
{
    // Landing on another adapter silently would invalidate any measurement.
    auto desc = warpDesc();
    desc.adapterSelector = "definitely-not-a-real-adapter";

    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, desc);
    ASSERT_FALSE(renderer.ok());
    EXPECT_EQ(renderer.error().code, r4d::ErrorCode::NotFound);
}

TEST_F(WarpDevice, NeverSubstitutesD3D12ForARequestedVulkanBackend)
{
    // A silent fallback would make a Vulkan benchmark secretly a D3D12 one.
    // Vulkan has no software device, so this uses the default adapter.
    auto desc = warpDesc();
    desc.adapterSelector.clear();

    auto renderer = r4d::createRenderer(r4d::BackendType::Vulkan, desc);
    if (!renderer)
    {
        // Disabled at build time or no suitable device: both are honest failures.
        EXPECT_NE(renderer.error().code, r4d::ErrorCode::Unknown);
        return;
    }
    EXPECT_EQ(renderer.value()->backend(), r4d::BackendType::Vulkan);
}

TEST_F(WarpDevice, RejectsAZeroSizedTarget)
{
    auto desc = warpDesc();
    desc.initialSize = r4d::UVec2{0, 0};

    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, desc);
    ASSERT_FALSE(renderer.ok());
    EXPECT_EQ(renderer.error().code, r4d::ErrorCode::InvalidArgument);
}

TEST_F(WarpDevice, RefusesToCaptureBeforeAnythingIsRendered)
{
    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, warpDesc());
    ASSERT_TRUE(renderer.ok()) << renderer.error().ToString();

    // A blank image would let an image test pass against nothing.
    auto capture = renderer.value()->captureFrame();
    EXPECT_FALSE(capture.ok());
}
