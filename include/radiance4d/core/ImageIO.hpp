// ImageIO.hpp
#pragma once

#include <radiance4d/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace r4d
{

    // Tightly packed RGBA8, top row first.
    struct ImageRgba8
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;

        std::size_t rowPitch() const { return static_cast<std::size_t>(width) * 4u; }
        std::size_t expectedBytes() const
        {
            return static_cast<std::size_t>(width) * height * 4u;
        }
        bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
    };

    Status writePng(const std::filesystem::path& path, const ImageRgba8& image);
    Result<ImageRgba8> readPng(const std::filesystem::path& path);

    // Decodes png/jpg/bmp/tga from memory, for glTF images held in a buffer view
    // or a data URI.
    Result<ImageRgba8> decodeImage(const std::byte* data, std::size_t size);

    // Box-filtered mip chain, largest first. Returns a single level for a 1x1.
    std::vector<ImageRgba8> generateMipChain(const ImageRgba8& image);

} // namespace r4d
