// Model.hpp
#pragma once

#include <radiance4d/core/Math.hpp>
#include <radiance4d/core/Result.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace r4d
{

    struct Vertex
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};   // w carries bitangent handedness
        Vec2 uv{0.0f, 0.0f};
    };

    struct Material
    {
        std::string name;
        Vec4 baseColour{1.0f, 1.0f, 1.0f, 1.0f};
        Vec3 emissive{0.0f, 0.0f, 0.0f};
        float metallic = 1.0f;
        float roughness = 1.0f;
    };

    // One draw: a contiguous index range sharing a material.
    struct Primitive
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::int32_t vertexOffset = 0;
        std::uint32_t materialIndex = 0;
        Mat4 transform{1.0f};
    };

    struct Bounds
    {
        Vec3 min{0.0f, 0.0f, 0.0f};
        Vec3 max{0.0f, 0.0f, 0.0f};

        Vec3 centre() const { return (min + max) * 0.5f; }
        float radius() const { return glm::length(max - min) * 0.5f; }
        bool valid() const { return max.x >= min.x; }
    };

    struct Model
    {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Primitive> primitives;
        std::vector<Material> materials;
        Bounds bounds;

        bool empty() const { return primitives.empty(); }
        std::size_t triangleCount() const { return indices.size() / 3; }
    };

    // Loads .gltf or .glb. Node transforms are flattened into the primitives.
    Result<Model> loadGltf(const std::filesystem::path& path);

    // Material grid used when no scene file is given.
    Model makeDemoScene(int columns = 7, int rows = 5, int sphereSegments = 48);

} // namespace r4d
