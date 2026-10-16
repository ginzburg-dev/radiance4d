// ImageIO.cpp
#include <radiance4d/core/ImageIO.hpp>

#include <format>

// stb implementations are compiled once, here. Its warnings are silenced
// locally rather than by relaxing /W4 for the whole target.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4267 4996 6262)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include <stb_image.h>
#include <stb_image_write.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace r4d
{

    Status writePng(const std::filesystem::path& path, const ImageRgba8& image)
    {
        if (image.empty())
        {
            return makeError(ErrorCode::InvalidArgument,
                             "writePng called with an empty image");
        }
        if (image.rgba.size() < image.expectedBytes())
        {
            return makeError(
                ErrorCode::InvalidArgument,
                std::format("writePng: {} bytes supplied for a {}x{} RGBA image that "
                            "needs {}",
                            image.rgba.size(), image.width, image.height,
                            image.expectedBytes()));
        }

        std::error_code ec;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        const int written =
            ::stbi_write_png(path.string().c_str(), static_cast<int>(image.width),
                             static_cast<int>(image.height), 4, image.rgba.data(),
                             static_cast<int>(image.rowPitch()));

        if (written == 0)
        {
            return makeError(
                ErrorCode::IoFailure,
                std::format("stbi_write_png failed for '{}'", path.string()));
        }
        return Status::Ok();
    }

    Result<ImageRgba8> readPng(const std::filesystem::path& path)
    {
        int width = 0;
        int height = 0;
        int channelsInFile = 0;

        // Force RGBA8 regardless of the source format.
        stbi_uc* pixels =
            ::stbi_load(path.string().c_str(), &width, &height, &channelsInFile, 4);
        if (pixels == nullptr)
        {
            const char* reason = ::stbi_failure_reason();
            return makeError(ErrorCode::IoFailure,
                             std::format("cannot read image '{}': {}", path.string(),
                                         reason != nullptr ? reason : "unknown"));
        }

        ImageRgba8 image;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.rgba.assign(pixels, pixels + image.expectedBytes());
        ::stbi_image_free(pixels);

        return image;
    }

    Result<ImageRgba8> decodeImage(const std::byte* data, std::size_t size)
    {
        if (data == nullptr || size == 0)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "decodeImage called with no data");
        }

        int width = 0;
        int height = 0;
        int channelsInFile = 0;
        stbi_uc* pixels = ::stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data),
                                                  static_cast<int>(size), &width, &height,
                                                  &channelsInFile, 4);
        if (pixels == nullptr)
        {
            const char* reason = ::stbi_failure_reason();
            return makeError(ErrorCode::IoFailure,
                             std::format("cannot decode image: {}",
                                         reason != nullptr ? reason : "unknown"));
        }

        ImageRgba8 image;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.rgba.assign(pixels, pixels + image.expectedBytes());
        ::stbi_image_free(pixels);
        return image;
    }

    std::vector<ImageRgba8> generateMipChain(const ImageRgba8& image)
    {
        std::vector<ImageRgba8> levels;
        if (image.empty())
        {
            return levels;
        }
        levels.push_back(image);

        while (levels.back().width > 1 || levels.back().height > 1)
        {
            const ImageRgba8& source = levels.back();
            ImageRgba8 next;
            next.width = source.width > 1 ? source.width / 2 : 1;
            next.height = source.height > 1 ? source.height / 2 : 1;
            next.rgba.resize(next.expectedBytes());

            for (std::uint32_t y = 0; y < next.height; ++y)
            {
                for (std::uint32_t x = 0; x < next.width; ++x)
                {
                    const std::uint32_t sx = x * 2;
                    const std::uint32_t sy = y * 2;
                    const std::uint32_t sx1 = (sx + 1 < source.width) ? sx + 1 : sx;
                    const std::uint32_t sy1 = (sy + 1 < source.height) ? sy + 1 : sy;

                    for (std::size_t c = 0; c < 4; ++c)
                    {
                        const auto at = [&](std::uint32_t px, std::uint32_t py)
                        {
                            return static_cast<int>(
                                source.rgba[(static_cast<std::size_t>(py) * source.width +
                                             px) *
                                                4 +
                                            c]);
                        };
                        const int sum =
                            at(sx, sy) + at(sx1, sy) + at(sx, sy1) + at(sx1, sy1);
                        next.rgba[(static_cast<std::size_t>(y) * next.width + x) * 4 +
                                  c] = static_cast<std::uint8_t>((sum + 2) / 4);
                    }
                }
            }
            levels.push_back(std::move(next));
        }
        return levels;
    }

} // namespace r4d
