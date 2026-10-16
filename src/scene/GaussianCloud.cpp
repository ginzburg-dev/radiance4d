// GaussianCloud.cpp
#include <radiance4d/scene/GaussianCloud.hpp>

#include <radiance4d/core/Log.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace r4d
{
    namespace
    {
        // Shortest rotation taking the local long axis onto a direction.
        Quat rotationTo(const Vec3& target)
        {
            const Vec3 from{1.0f, 0.0f, 0.0f};
            const Vec3 to = glm::normalize(target);
            const Vec3 axis = glm::cross(from, to);
            const float sine = glm::length(axis);
            if (sine < 1e-6f)
            {
                return glm::dot(from, to) > 0.0f
                           ? Quat{1.0f, 0.0f, 0.0f, 0.0f}
                           : glm::angleAxis(kPi, Vec3{0.0f, 1.0f, 0.0f});
            }
            return glm::angleAxis(std::atan2(sine, glm::dot(from, to)), axis / sine);
        }

        // Nearest-rank quantile, in place: always one of the samples, so the
        // result is a distance the cloud actually has. nth_element keeps it
        // linear, which matters at a million splats.
        float quantileInPlace(std::vector<float>& values, float quantile)
        {
            const float position =
                std::clamp(quantile, 0.0f, 1.0f) * static_cast<float>(values.size() - 1);
            const auto rank = static_cast<std::size_t>(position);
            std::nth_element(values.begin(), values.begin() + rank, values.end());
            return values[rank];
        }
    } // namespace

    Sphere robustFramingSphere(const std::vector<Gaussian>& gaussians, float quantile)
    {
        if (gaussians.empty())
        {
            return Sphere{};
        }

        std::vector<float> scratch(gaussians.size());

        // The median is the centre: on `train` it lands 0.07 units from the
        // median of the 301 training camera positions, while the mean is dragged
        // out by the tail and the bounds' centre misses by 35 units.
        Vec3 centre{0.0f};
        for (int axis = 0; axis < 3; ++axis)
        {
            std::transform(gaussians.begin(), gaussians.end(), scratch.begin(),
                           [axis](const Gaussian& g) { return g.position[axis]; });
            centre[axis] = quantileInPlace(scratch, 0.5f);
        }

        std::transform(gaussians.begin(), gaussians.end(), scratch.begin(),
                       [&centre](const Gaussian& g)
                       { return glm::length(g.position - centre); });
        const float radius = quantileInPlace(scratch, quantile);

        std::transform(gaussians.begin(), gaussians.end(), scratch.begin(),
                       [](const Gaussian& g)
                       { return std::max({g.scale.x, g.scale.y, g.scale.z}); });
        const float padding = quantileInPlace(scratch, 0.5f) * 3.0f;

        return Sphere{centre, radius + padding};
    }

    void GaussianCloud::recomputeBounds()
    {
        bool first = true;
        for (const Gaussian& g : gaussians)
        {
            const float extent = std::max({g.scale.x, g.scale.y, g.scale.z}) * 3.0f;
            const Vec3 low = g.position - Vec3{extent};
            const Vec3 high = g.position + Vec3{extent};
            if (first)
            {
                bounds.min = low;
                bounds.max = high;
                first = false;
            }
            else
            {
                bounds.min = glm::min(bounds.min, low);
                bounds.max = glm::max(bounds.max, high);
            }
        }

        framing = robustFramingSphere(gaussians);
    }

    GaussianCloud makeProceduralGaussianCloud(std::uint32_t count, std::uint32_t seed)
    {
        // A fixed engine and seed: the image-regression and parity tests need the
        // same cloud on every machine and every run.
        std::mt19937 rng{seed};
        std::uniform_real_distribution<float> unit{0.0f, 1.0f};
        std::uniform_real_distribution<float> signed01{-1.0f, 1.0f};

        GaussianCloud cloud;
        cloud.gaussians.reserve(count);

        // Two interleaved shells plus a dusting of haze, which gives overlapping
        // splats at a range of depths so the compositing order actually matters.
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(count);

            Gaussian g;
            bool oriented = false;

            if (i % 4 == 0)
            {
                const float radius = 1.9f + signed01(rng) * 0.08f;
                const float theta = unit(rng) * 2.0f * kPi;
                const float phi = std::acos(signed01(rng));
                g.position = Vec3{radius * std::sin(phi) * std::cos(theta),
                                  radius * std::cos(phi) * 0.55f,
                                  radius * std::sin(phi) * std::sin(theta)};
                setConstantColour(g, Vec3{0.25f + 0.65f * unit(rng), 0.45f, 0.95f});
                g.opacity = 0.35f + 0.4f * unit(rng);
                g.scale = Vec3{0.03f, 0.03f, 0.03f} * (0.6f + unit(rng));
            }
            else if (i % 4 == 1)
            {
                // A ribbon, so there is elongated anisotropic geometry to project.
                // The splats lie along its tangent, which makes a wrong
                // covariance projection visible rather than plausible.
                const float angle = t * 18.0f * kPi;
                const float radius = 0.6f + t * 1.1f;
                g.position = Vec3{radius * std::cos(angle), (t - 0.5f) * 1.8f,
                                  radius * std::sin(angle)};
                setConstantColour(g, Vec3{0.95f, 0.45f + 0.4f * t, 0.2f});
                g.opacity = 0.55f + 0.35f * unit(rng);
                g.scale = Vec3{0.11f, 0.018f, 0.018f};

                const float dAngle = 18.0f * kPi;
                g.rotation = rotationTo(
                    Vec3{1.1f * std::cos(angle) - radius * dAngle * std::sin(angle), 1.8f,
                         1.1f * std::sin(angle) + radius * dAngle * std::cos(angle)});
                oriented = true;
            }
            else
            {
                const float radius = 1.45f * std::cbrt(unit(rng));
                const float theta = unit(rng) * 2.0f * kPi;
                const float phi = std::acos(signed01(rng));
                g.position =
                    Vec3{radius * std::sin(phi) * std::cos(theta), radius * std::cos(phi),
                         radius * std::sin(phi) * std::sin(theta)};
                setConstantColour(g,
                                  Vec3{0.55f + 0.4f * unit(rng), 0.6f + 0.3f * unit(rng),
                                       0.7f + 0.3f * unit(rng)});
                g.opacity = 0.04f + 0.10f * unit(rng);
                g.scale = Vec3{0.05f, 0.05f, 0.05f} * (0.5f + 1.5f * unit(rng));
            }

            if (!oriented)
            {
                const Vec3 axis =
                    glm::normalize(Vec3{signed01(rng), signed01(rng), signed01(rng)} +
                                   Vec3{0.0f, 0.001f, 0.0f});
                g.rotation = glm::angleAxis(unit(rng) * 2.0f * kPi, axis);
            }

            cloud.gaussians.push_back(g);
        }

        cloud.recomputeBounds();

        R4D_INFO("Gaussians: {} procedural splats", cloud.count());
        return cloud;
    }

} // namespace r4d
