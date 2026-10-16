// Environment.cpp
#include <radiance4d/scene/Model.hpp>

#include <radiance4d/core/Log.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace r4d
{
    namespace
    {
        Vec3 skyRadiance(Vec3 direction, Vec3 sunDirection)
        {
            const float up = glm::clamp(direction.y, -1.0f, 1.0f);

            // Sky above, dimmer bounce below, with a soft horizon band.
            const Vec3 zenith{0.18f, 0.32f, 0.62f};
            const Vec3 horizon{0.62f, 0.66f, 0.72f};
            const Vec3 ground{0.10f, 0.088f, 0.078f};

            Vec3 colour;
            if (up >= 0.0f)
            {
                colour = glm::mix(horizon, zenith, std::pow(up, 0.55f));
            }
            else
            {
                colour = glm::mix(horizon, ground, std::pow(-up, 0.35f));
            }

            // A soft sun rather than a hard disc. A near-singular gradient makes
            // mirror reflections alias, and it amplifies sub-texel filtering
            // differences between the two backends into visible divergence.
            const float cosAngle =
                glm::clamp(glm::dot(direction, sunDirection), -1.0f, 1.0f);
            const float glow = std::pow(std::max(cosAngle, 0.0f), 24.0f);
            const float core = std::pow(std::max(cosAngle, 0.0f), 220.0f);
            colour += Vec3{1.0f, 0.93f, 0.82f} * glow * 1.1f;
            colour += Vec3{1.0f, 0.95f, 0.85f} * core * 2.2f;

            return colour;
        }

        Vec3 equirectDirection(float u, float v)
        {
            const float phi = (u - 0.5f) * 2.0f * kPi;
            const float theta = v * kPi;
            const float sinTheta = std::sin(theta);
            return Vec3{sinTheta * std::cos(phi), std::cos(theta),
                        sinTheta * std::sin(phi)};
        }

        std::uint8_t encode(float linear)
        {
            // Reinhard before the 8-bit encode: the sun is far brighter than 1.0
            // and would otherwise clip to a flat white disc.
            const float mapped = linear / (1.0f + linear);
            const float srgb = std::pow(glm::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f);
            return static_cast<std::uint8_t>(
                glm::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    } // namespace

    TextureImage makeProceduralSky(std::uint32_t width, std::uint32_t height)
    {
        const Vec3 sunDirection = glm::normalize(Vec3{-0.4f, 0.8f, -0.45f});

        TextureImage texture;
        texture.srgb = true;
        texture.image.width = width;
        texture.image.height = height;
        texture.image.rgba.resize(texture.image.expectedBytes());

        for (std::uint32_t y = 0; y < height; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const float u =
                    (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                const Vec3 colour = skyRadiance(equirectDirection(u, v), sunDirection);
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
                texture.image.rgba[i + 0] = encode(colour.x);
                texture.image.rgba[i + 1] = encode(colour.y);
                texture.image.rgba[i + 2] = encode(colour.z);
                texture.image.rgba[i + 3] = 255;
            }
        }
        return texture;
    }

    TextureImage makeIrradianceMap(std::uint32_t width, std::uint32_t height)
    {
        const Vec3 sunDirection = glm::normalize(Vec3{-0.4f, 0.8f, -0.45f});

        // Cosine convolution of the same sky. Done at a low resolution on the CPU,
        // which is cheap enough at 64x32 and avoids a compute pass in two backends.
        constexpr std::uint32_t kSamplesPhi = 64;
        constexpr std::uint32_t kSamplesTheta = 32;

        struct Sample
        {
            Vec3 direction;
            Vec3 radiance;
            float weight;
        };
        std::vector<Sample> samples;
        samples.reserve(kSamplesPhi * kSamplesTheta);

        for (std::uint32_t t = 0; t < kSamplesTheta; ++t)
        {
            const float theta =
                (static_cast<float>(t) + 0.5f) / static_cast<float>(kSamplesTheta) * kPi;
            const float sinTheta = std::sin(theta);
            for (std::uint32_t p = 0; p < kSamplesPhi; ++p)
            {
                const float phi = (static_cast<float>(p) + 0.5f) /
                                      static_cast<float>(kSamplesPhi) * 2.0f * kPi -
                                  kPi;
                const Vec3 direction{sinTheta * std::cos(phi), std::cos(theta),
                                     sinTheta * std::sin(phi)};
                samples.push_back(
                    {direction, skyRadiance(direction, sunDirection), sinTheta});
            }
        }

        TextureImage texture;
        texture.srgb = true;
        texture.image.width = width;
        texture.image.height = height;
        texture.image.rgba.resize(texture.image.expectedBytes());

        const float solidAngle = (kPi / kSamplesTheta) * (2.0f * kPi / kSamplesPhi);

        for (std::uint32_t y = 0; y < height; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const float u =
                    (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                const Vec3 normal = equirectDirection(u, v);

                Vec3 total{0.0f};
                for (const Sample& sample : samples)
                {
                    const float cosine = glm::dot(normal, sample.direction);
                    if (cosine > 0.0f)
                    {
                        total += sample.radiance * cosine * sample.weight * solidAngle;
                    }
                }
                total /= kPi; // Lambert

                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
                texture.image.rgba[i + 0] = encode(total.x);
                texture.image.rgba[i + 1] = encode(total.y);
                texture.image.rgba[i + 2] = encode(total.z);
                texture.image.rgba[i + 3] = 255;
            }
        }

        R4D_INFO("Environment: {}x{} sky, {}x{} irradiance", 0, 0, width, height);
        return texture;
    }

    void attachDefaultEnvironment(Model& model)
    {
        model.environment = makeProceduralSky(512, 256);
        model.irradiance = makeIrradianceMap(32, 16);
    }

} // namespace r4d
