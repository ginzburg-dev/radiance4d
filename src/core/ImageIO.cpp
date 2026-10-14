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

        const int written = ::stbi_write_png(
            path.string().c_str(), static_cast<int>(image.width),
            static_cast<int>(image.height), 4, image.rgba.data(),
            static_cast<int>(image.rowPitch()));

        if (written == 0)
        {
            return makeError(ErrorCode::IoFailure,
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

} // namespace r4d
