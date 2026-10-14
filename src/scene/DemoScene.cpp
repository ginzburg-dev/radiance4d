// DemoScene.cpp
#include <radiance4d/scene/Model.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace r4d
{
    namespace
    {
        void addSphere(Model& model, Vec3 centre, float radius, std::uint32_t materialIndex,
                       int segments, int rings)
        {
            const auto firstVertex = static_cast<std::int32_t>(model.vertices.size());
            const auto firstIndex = static_cast<std::uint32_t>(model.indices.size());

            for (int y = 0; y <= rings; ++y)
            {
                const float v = static_cast<float>(y) / static_cast<float>(rings);
                const float phi = v * kPi;
                for (int x = 0; x <= segments; ++x)
                {
                    const float u = static_cast<float>(x) / static_cast<float>(segments);
                    const float theta = u * 2.0f * kPi;

                    const Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                                      std::sin(phi) * std::sin(theta)};

                    Vertex vertex;
                    vertex.position = centre + normal * radius;
                    vertex.normal = normal;
                    vertex.tangent = Vec4{-std::sin(theta), 0.0f, std::cos(theta), 1.0f};
                    vertex.uv = Vec2{u, v};
                    model.vertices.push_back(vertex);
                }
            }

            for (int y = 0; y < rings; ++y)
            {
                for (int x = 0; x < segments; ++x)
                {
                    const std::uint32_t a = static_cast<std::uint32_t>(y * (segments + 1) + x);
                    const std::uint32_t b = a + static_cast<std::uint32_t>(segments + 1);
                    model.indices.insert(model.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
                }
            }

            Primitive primitive;
            primitive.firstIndex = firstIndex;
            primitive.indexCount = static_cast<std::uint32_t>(model.indices.size()) - firstIndex;
            primitive.vertexOffset = firstVertex;
            primitive.materialIndex = materialIndex;
            model.primitives.push_back(primitive);
        }

        void addGround(Model& model, float extent, float y, std::uint32_t materialIndex)
        {
            const auto firstVertex = static_cast<std::int32_t>(model.vertices.size());
            const auto firstIndex = static_cast<std::uint32_t>(model.indices.size());

            const Vec3 corners[4] = {{-extent, y, -extent},
                                     {extent, y, -extent},
                                     {extent, y, extent},
                                     {-extent, y, extent}};
            const Vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

            for (int i = 0; i < 4; ++i)
            {
                Vertex vertex;
                vertex.position = corners[i];
                vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
                vertex.tangent = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
                vertex.uv = uvs[i] * 8.0f;
                model.vertices.push_back(vertex);
            }

            model.indices.insert(model.indices.end(), {0u, 2u, 1u, 0u, 3u, 2u});

            Primitive primitive;
            primitive.firstIndex = firstIndex;
            primitive.indexCount = 6;
            primitive.vertexOffset = firstVertex;
            primitive.materialIndex = materialIndex;
            model.primitives.push_back(primitive);
        }
    } // namespace

    Model makeDemoScene(int columns, int rows, int sphereSegments)
    {
        Model model;

        const int kColumns = std::max(columns, 2);
        const int kRows = std::max(rows, 2);
        const int kSegments = std::max(sphereSegments, 6);
        const int kRings = std::max(kSegments * 2 / 3, 4);
        constexpr float kSpacing = 1.1f;
        constexpr float kRadius = 0.45f;

        Material ground;
        ground.name = "ground";
        ground.baseColour = Vec4{0.06f, 0.07f, 0.09f, 1.0f};
        ground.metallic = 0.1f;
        ground.roughness = 0.45f;
        model.materials.push_back(ground);

        const float width = (kColumns - 1) * kSpacing;
        const float height = (kRows - 1) * kSpacing;

        // Roughness varies across, metallic down: the usual material grid.
        for (int row = 0; row < kRows; ++row)
        {
            for (int column = 0; column < kColumns; ++column)
            {
                Material material;
                material.name = std::format("sphere_{}_{}", row, column);
                material.baseColour = Vec4{0.92f, 0.55f, 0.28f, 1.0f};
                material.metallic =
                    static_cast<float>(row) / static_cast<float>(kRows - 1);
                material.roughness = glm::clamp(
                    static_cast<float>(column) / static_cast<float>(kColumns - 1), 0.06f, 1.0f);
                model.materials.push_back(material);

                const Vec3 centre{column * kSpacing - width * 0.5f,
                                  height * 0.5f - row * kSpacing, 0.0f};
                addSphere(model, centre, kRadius,
                          static_cast<std::uint32_t>(model.materials.size() - 1),
                          kSegments, kRings);
            }
        }

        // Bounds come from the spheres only: the ground is a backdrop, and
        // including it would frame the camera on empty floor.
        const std::size_t subjectVertexCount = model.vertices.size();

        addGround(model, width * 0.75f, height * -0.5f - kRadius * 1.6f, 0);

        bool first = true;
        for (std::size_t i = 0; i < subjectVertexCount; ++i)
        {
            const Vertex& vertex = model.vertices[i];
            if (first)
            {
                model.bounds.min = vertex.position;
                model.bounds.max = vertex.position;
                first = false;
            }
            else
            {
                model.bounds.min = glm::min(model.bounds.min, vertex.position);
                model.bounds.max = glm::max(model.bounds.max, vertex.position);
            }
        }

        return model;
    }

} // namespace r4d
