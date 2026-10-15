// SplatRender_test.cpp
//
// The splat pass on WARP. Structural assertions first, because a reference
// image produced by the code under test cannot by itself prove the covariance
// projection is right.

#include <radiance4d/core/ImageIO.hpp>
#include <radiance4d/core/Log.hpp>
#include <radiance4d/graphics/Renderer.hpp>
#include <radiance4d/scene/Camera.hpp>
#include <radiance4d/scene/GaussianCloud.hpp>

#include "ImageCompare.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace
{
    constexpr std::uint32_t kWidth = 640;
    constexpr std::uint32_t kHeight = 360;
    constexpr std::uint32_t kSplats = 4000;
    constexpr double kFixedStep = 1.0 / 60.0;

    constexpr std::uint8_t kClearR = 11;
    constexpr std::uint8_t kClearG = 12;
    constexpr std::uint8_t kClearB = 16;

    std::filesystem::path referencePath()
    {
        return std::filesystem::path{R4D_TEST_DATA_DIR} / "references" / "warp" /
               "splats_640x360.png";
    }

    r4d::RendererDesc headlessWarpDesc()
    {
        r4d::RendererDesc desc;
        desc.windowHandle = nullptr;
        desc.initialSize = r4d::UVec2{kWidth, kHeight};
        desc.adapterSelector = "warp";
        desc.enableDebugLayer = true;
        desc.fixedTimeStep = kFixedStep;
        return desc;
    }

    r4d::Result<r4d::FrameCapture> renderSplats(std::uint32_t splats = kSplats,
                                                int frames = 4)
    {
        auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, headlessWarpDesc());
        if (!renderer)
        {
            return renderer.error();
        }
        auto& gpu = *renderer.value();

        const r4d::GaussianCloud cloud = r4d::makeProceduralGaussianCloud(splats, 1337);
        if (auto status = gpu.uploadGaussians(cloud); !status)
        {
            return status.error();
        }

        r4d::Camera camera;
        camera.setAspect(static_cast<float>(kWidth) / static_cast<float>(kHeight));
        camera.focusOn(cloud.bounds.centre(), cloud.bounds.radius());
        gpu.setCamera(
            r4d::CameraParams{camera.view(), camera.projection(), camera.position()});

        for (int i = 0; i < frames; ++i)
        {
            if (auto status = gpu.renderFrame(); !status)
            {
                return status.error();
            }
        }
        if (gpu.validationErrorCount() != 0)
        {
            return r4d::makeError(r4d::ErrorCode::ApiCallFailed,
                                  "debug-layer errors were reported while splatting");
        }
        return gpu.captureFrame();
    }

    std::uint8_t channelAt(const r4d::ImageRgba8& image, std::uint32_t x, std::uint32_t y,
                           std::size_t channel)
    {
        return image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + channel];
    }

    double coverage(const r4d::ImageRgba8& image)
    {
        std::uint64_t covered = 0;
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                if (channelAt(image, x, y, 0) != kClearR ||
                    channelAt(image, x, y, 1) != kClearG ||
                    channelAt(image, x, y, 2) != kClearB)
                {
                    ++covered;
                }
            }
        }
        return static_cast<double>(covered) /
               static_cast<double>(static_cast<std::uint64_t>(image.width) * image.height);
    }

    class SplatRender : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            r4d::Logger::instance().init(r4d::LogLevel::Warning, true, {});
            r4d::Logger::instance().resetCounters();
        }
    };
} // namespace

TEST_F(SplatRender, DrawsTheCloudWithoutAMesh)
{
    auto capture = renderSplats();
    ASSERT_TRUE(capture.ok()) << capture.error().ToString();
    const auto& image = capture.value();

    EXPECT_EQ(image.width, kWidth);
    EXPECT_EQ(image.height, kHeight);

    const double covered = coverage(image);
    EXPECT_GT(covered, 0.02) << "the splat pass drew nothing";
    EXPECT_LT(covered, 0.9) << "the splats cover the whole frame; is the radius sane?";

    // The cloud is framed, so it must not reach the corners.
    EXPECT_EQ(channelAt(image, 0, 0, 0), kClearR);
    EXPECT_EQ(channelAt(image, kWidth - 1, kHeight - 1, 2), kClearB);
}

TEST_F(SplatRender, MoreSplatsCoverMore)
{
    // Guards against a pass that draws a constant blob regardless of the input.
    auto sparse = renderSplats(400);
    ASSERT_TRUE(sparse.ok()) << sparse.error().ToString();
    auto dense = renderSplats(8000);
    ASSERT_TRUE(dense.ok()) << dense.error().ToString();

    EXPECT_GT(coverage(dense.value()), coverage(sparse.value()));
}

TEST_F(SplatRender, IsReproducible)
{
    auto first = renderSplats();
    ASSERT_TRUE(first.ok()) << first.error().ToString();
    auto second = renderSplats();
    ASSERT_TRUE(second.ok()) << second.error().ToString();

    // The per-frame depth sort must not reorder equal keys differently.
    EXPECT_EQ(first.value().rgba, second.value().rgba);
}

TEST_F(SplatRender, AnEmptyCloudClearsThePass)
{
    auto renderer = r4d::createRenderer(r4d::BackendType::D3D12, headlessWarpDesc());
    ASSERT_TRUE(renderer.ok()) << renderer.error().ToString();
    auto& gpu = *renderer.value();

    ASSERT_TRUE(gpu.uploadGaussians(r4d::makeProceduralGaussianCloud(256, 5)).ok());
    ASSERT_TRUE(gpu.uploadGaussians(r4d::GaussianCloud{}).ok());
    ASSERT_TRUE(gpu.renderFrame().ok());

    auto capture = gpu.captureFrame();
    ASSERT_TRUE(capture.ok()) << capture.error().ToString();
    EXPECT_EQ(coverage(capture.value()), 0.0);
    EXPECT_EQ(gpu.validationErrorCount(), 0u);
}

TEST_F(SplatRender, MatchesTheStoredWarpReference)
{
    const auto reference = referencePath();

    auto capture = renderSplats();
    ASSERT_TRUE(capture.ok()) << capture.error().ToString();

    if (!std::filesystem::exists(reference))
    {
        const auto candidate = reference.string() + ".actual.png";
        (void)r4d::writePng(candidate, capture.value());
        FAIL() << "reference image missing: " << reference.string()
               << "\nA candidate was written to " << candidate
               << "\nInspect it, and if it is correct rename it to the reference path.";
    }

    auto expected = r4d::readPng(reference);
    ASSERT_TRUE(expected.ok()) << expected.error().ToString();

    constexpr std::uint32_t kPerPixelTolerance = 2;
    const auto difference = r4d::testing::compareImages(capture.value(), expected.value(),
                                                        kPerPixelTolerance);

    if (!difference.comparable || difference.maxChannelDelta > 4 || difference.rmse > 1.0 ||
        difference.differingFraction() > 0.001)
    {
        r4d::testing::writeFailureArtefacts(reference, capture.value(), expected.value());
        FAIL() << "splat render differs from the stored WARP reference: "
               << difference.message << "\nWrote .actual.png and .diff.png beside "
               << reference.string();
    }

    SUCCEED() << difference.message;
}
