// ImageIO_test.cpp

#include <radiance4d/core/ImageIO.hpp>

#include "ImageCompare.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace
{
    r4d::ImageRgba8 makeTestImage(std::uint32_t width, std::uint32_t height)
    {
        r4d::ImageRgba8 image;
        image.width = width;
        image.height = height;
        image.rgba.resize(image.expectedBytes());
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
                image.rgba[i + 0] = static_cast<std::uint8_t>(x * 7 + 1);
                image.rgba[i + 1] = static_cast<std::uint8_t>(y * 5 + 2);
                image.rgba[i + 2] = static_cast<std::uint8_t>((x + y) * 3);
                image.rgba[i + 3] = 255;
            }
        }
        return image;
    }

    std::filesystem::path tempFile(const char* name)
    {
        return std::filesystem::temp_directory_path() / name;
    }
} // namespace

TEST(ImageIO, RoundTripsPixelsExactly)
{
    // PNG is lossless; a mismatch means a layout or stride bug.
    const auto image = makeTestImage(17, 11);
    const auto path = tempFile("r4d_roundtrip.png");

    ASSERT_TRUE(r4d::writePng(path, image).ok());
    auto loaded = r4d::readPng(path);
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();

    EXPECT_EQ(loaded.value().width, image.width);
    EXPECT_EQ(loaded.value().height, image.height);
    EXPECT_EQ(loaded.value().rgba, image.rgba);

    std::filesystem::remove(path);
}

TEST(ImageIO, CreatesMissingParentDirectories)
{
    const auto directory = tempFile("r4d_nested_dir");
    const auto path = directory / "deeper" / "image.png";
    std::filesystem::remove_all(directory);

    ASSERT_TRUE(r4d::writePng(path, makeTestImage(4, 4)).ok());
    EXPECT_TRUE(std::filesystem::exists(path));

    std::filesystem::remove_all(directory);
}

TEST(ImageIO, RejectsAnEmptyImage)
{
    EXPECT_FALSE(r4d::writePng(tempFile("r4d_empty.png"), r4d::ImageRgba8{}).ok());
}

TEST(ImageIO, RejectsTruncatedPixelData)
{
    r4d::ImageRgba8 image = makeTestImage(8, 8);
    image.rgba.resize(image.rgba.size() / 2);
    EXPECT_FALSE(r4d::writePng(tempFile("r4d_truncated.png"), image).ok());
}

TEST(ImageIO, ReportsAMissingFile)
{
    auto loaded = r4d::readPng(tempFile("r4d_does_not_exist_at_all.png"));
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::IoFailure);
}

TEST(ImageCompare, ReportsIdenticalImagesAsIdentical)
{
    const auto image = makeTestImage(9, 6);
    const auto difference = r4d::testing::compareImages(image, image);

    EXPECT_TRUE(difference.comparable);
    EXPECT_EQ(difference.maxChannelDelta, 0u);
    EXPECT_DOUBLE_EQ(difference.rmse, 0.0);
    EXPECT_EQ(difference.differingPixels, 0u);
}

TEST(ImageCompare, DetectsASingleChangedChannel)
{
    const auto reference = makeTestImage(9, 6);
    auto modified = reference;
    modified.rgba[4 * 4 + 1] = static_cast<std::uint8_t>(reference.rgba[4 * 4 + 1] + 30);

    const auto difference = r4d::testing::compareImages(modified, reference);
    EXPECT_TRUE(difference.comparable);
    EXPECT_EQ(difference.maxChannelDelta, 30u);
    EXPECT_EQ(difference.differingPixels, 1u);
    EXPECT_GT(difference.rmse, 0.0);
}

TEST(ImageCompare, HonoursThePerPixelTolerance)
{
    const auto reference = makeTestImage(5, 5);
    auto modified = reference;
    modified.rgba[0] = static_cast<std::uint8_t>(reference.rgba[0] + 2);

    EXPECT_EQ(r4d::testing::compareImages(modified, reference, 2).differingPixels, 0u);
    EXPECT_EQ(r4d::testing::compareImages(modified, reference, 1).differingPixels, 1u);
}

TEST(ImageCompare, RefusesToCompareDifferentSizes)
{
    const auto difference =
        r4d::testing::compareImages(makeTestImage(4, 4), makeTestImage(5, 4));
    EXPECT_FALSE(difference.comparable);
    EXPECT_NE(difference.message.find("size mismatch"), std::string::npos);
}
