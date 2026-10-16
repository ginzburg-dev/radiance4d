// ImageCompare.hpp
#pragma once

#include <radiance4d/core/ImageIO.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

namespace r4d::testing
{

    struct ImageDifference
    {
        bool comparable = false;
        std::uint32_t maxChannelDelta = 0;
        double rmse = 0.0;
        std::uint64_t differingPixels = 0;
        std::uint64_t totalPixels = 0;
        std::string message;

        double differingFraction() const
        {
            return totalPixels == 0 ? 0.0
                                    : static_cast<double>(differingPixels) /
                                          static_cast<double>(totalPixels);
        }
    };

    // Reports both worst channel deviation and RMSE: one stray pixel and a
    // uniformly slightly-off image are different failures.
    inline ImageDifference compareImages(const ImageRgba8& actual,
                                         const ImageRgba8& expected,
                                         std::uint32_t perPixelTolerance = 0)
    {
        ImageDifference difference;

        if (actual.width != expected.width || actual.height != expected.height)
        {
            difference.message =
                std::format("size mismatch: actual {}x{}, expected {}x{}", actual.width,
                            actual.height, expected.width, expected.height);
            return difference;
        }
        if (actual.rgba.size() < actual.expectedBytes() ||
            expected.rgba.size() < expected.expectedBytes())
        {
            difference.message = "truncated pixel data";
            return difference;
        }

        difference.comparable = true;
        difference.totalPixels = static_cast<std::uint64_t>(actual.width) * actual.height;

        double squaredSum = 0.0;
        const std::size_t channels = actual.expectedBytes();

        for (std::size_t pixel = 0; pixel < difference.totalPixels; ++pixel)
        {
            std::uint32_t worstForPixel = 0;
            for (std::size_t c = 0; c < 4; ++c)
            {
                const std::size_t i = pixel * 4 + c;
                if (i >= channels)
                {
                    break;
                }
                const int delta =
                    static_cast<int>(actual.rgba[i]) - static_cast<int>(expected.rgba[i]);
                const std::uint32_t magnitude =
                    static_cast<std::uint32_t>(delta < 0 ? -delta : delta);
                worstForPixel = std::max(worstForPixel, magnitude);
                squaredSum += static_cast<double>(delta) * static_cast<double>(delta);
            }
            difference.maxChannelDelta =
                std::max(difference.maxChannelDelta, worstForPixel);
            if (worstForPixel > perPixelTolerance)
            {
                ++difference.differingPixels;
            }
        }

        difference.rmse =
            channels == 0 ? 0.0 : std::sqrt(squaredSum / static_cast<double>(channels));
        difference.message = std::format(
            "max channel delta {}, RMSE {:.4f}, {} of {} pixels beyond tolerance {}",
            difference.maxChannelDelta, difference.rmse, difference.differingPixels,
            difference.totalPixels, perPixelTolerance);
        return difference;
    }

    // Amplified absolute difference, for eyeballing a failure.
    inline ImageRgba8 makeDifferenceImage(const ImageRgba8& actual,
                                          const ImageRgba8& expected,
                                          int amplification = 8)
    {
        ImageRgba8 diff;
        if (actual.width != expected.width || actual.height != expected.height)
        {
            return diff;
        }
        diff.width = actual.width;
        diff.height = actual.height;
        diff.rgba.resize(diff.expectedBytes());

        for (std::size_t pixel = 0; pixel < diff.expectedBytes() / 4; ++pixel)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                const std::size_t i = pixel * 4 + c;
                const int delta = std::abs(static_cast<int>(actual.rgba[i]) -
                                           static_cast<int>(expected.rgba[i]));
                diff.rgba[i] =
                    static_cast<std::uint8_t>(std::min(255, delta * amplification));
            }
            diff.rgba[pixel * 4 + 3] = 255;
        }
        return diff;
    }

    // Writes actual and diff next to the reference.
    inline void writeFailureArtefacts(const std::filesystem::path& referencePath,
                                      const ImageRgba8& actual,
                                      const ImageRgba8& expected)
    {
        auto base = referencePath;
        base.replace_extension();

        const auto actualPath = base.string() + ".actual.png";
        if (auto status = writePng(actualPath, actual); !status)
        {
            return;
        }

        const auto diff = makeDifferenceImage(actual, expected);
        if (!diff.empty())
        {
            (void)writePng(base.string() + ".diff.png", diff);
        }
    }

} // namespace r4d::testing
