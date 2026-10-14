// GltfLoader.cpp
#include <radiance4d/scene/Model.hpp>

#include <radiance4d/core/Log.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4996)
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>
#include <format>

namespace r4d
{
    namespace
    {
        Mat4 nodeTransform(const cgltf_node* node)
        {
            float matrix[16];
            cgltf_node_transform_world(node, matrix);
            return glm::make_mat4(matrix);
        }

        const char* describe(cgltf_result result)
        {
            switch (result)
            {
                case cgltf_result_data_too_short:  return "data too short";
                case cgltf_result_unknown_format:  return "unknown format";
                case cgltf_result_invalid_json:    return "invalid json";
                case cgltf_result_invalid_gltf:    return "invalid gltf";
                case cgltf_result_out_of_memory:   return "out of memory";
                case cgltf_result_file_not_found:  return "file not found";
                case cgltf_result_io_error:        return "io error";
                default:                           return "failed";
            }
        }

        void expandBounds(Bounds& bounds, Vec3 point, bool& first)
        {
            if (first)
            {
                bounds.min = point;
                bounds.max = point;
                first = false;
                return;
            }
            bounds.min = glm::min(bounds.min, point);
            bounds.max = glm::max(bounds.max, point);
        }

        // Flat shading fallback when a mesh ships no normals.
        void generateNormals(Model& model, std::size_t firstVertex, std::size_t firstIndex,
                             std::size_t indexCount)
        {
            for (std::size_t i = 0; i + 2 < indexCount; i += 3)
            {
                const std::uint32_t a = model.indices[firstIndex + i] + static_cast<std::uint32_t>(firstVertex);
                const std::uint32_t b = model.indices[firstIndex + i + 1] + static_cast<std::uint32_t>(firstVertex);
                const std::uint32_t c = model.indices[firstIndex + i + 2] + static_cast<std::uint32_t>(firstVertex);
                const Vec3 normal = glm::normalize(glm::cross(
                    model.vertices[b].position - model.vertices[a].position,
                    model.vertices[c].position - model.vertices[a].position));
                model.vertices[a].normal = normal;
                model.vertices[b].normal = normal;
                model.vertices[c].normal = normal;
            }
        }
    } // namespace

    Result<Model> loadGltf(const std::filesystem::path& path)
    {
        cgltf_options options{};
        cgltf_data* data = nullptr;

        const std::string pathText = path.string();
        cgltf_result result = cgltf_parse_file(&options, pathText.c_str(), &data);
        if (result != cgltf_result_success)
        {
            return makeError(ErrorCode::IoFailure,
                             std::format("cannot parse '{}': {}", pathText, describe(result)));
        }

        result = cgltf_load_buffers(&options, data, pathText.c_str());
        if (result != cgltf_result_success)
        {
            cgltf_free(data);
            return makeError(ErrorCode::IoFailure,
                             std::format("cannot load buffers for '{}': {}", pathText,
                                         describe(result)));
        }

        if (cgltf_validate(data) != cgltf_result_success)
        {
            cgltf_free(data);
            return makeError(ErrorCode::IoFailure,
                             std::format("'{}' failed glTF validation", pathText));
        }

        Model model;
        bool firstPoint = true;

        model.materials.push_back(Material{"default", Vec4{0.8f, 0.8f, 0.8f, 1.0f},
                                           Vec3{0.0f}, 0.0f, 0.6f});

        for (cgltf_size i = 0; i < data->materials_count; ++i)
        {
            const cgltf_material& source = data->materials[i];
            Material material;
            material.name = source.name != nullptr ? source.name : "material";
            if (source.has_pbr_metallic_roughness)
            {
                const auto& pbr = source.pbr_metallic_roughness;
                material.baseColour = Vec4{pbr.base_color_factor[0], pbr.base_color_factor[1],
                                           pbr.base_color_factor[2], pbr.base_color_factor[3]};
                material.metallic = pbr.metallic_factor;
                material.roughness = pbr.roughness_factor;
            }
            material.emissive = Vec3{source.emissive_factor[0], source.emissive_factor[1],
                                     source.emissive_factor[2]};
            model.materials.push_back(material);
        }

        for (cgltf_size n = 0; n < data->nodes_count; ++n)
        {
            const cgltf_node& node = data->nodes[n];
            if (node.mesh == nullptr)
            {
                continue;
            }
            const Mat4 transform = nodeTransform(&node);
            const Mat4 normalMatrix = glm::transpose(glm::inverse(transform));

            for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
            {
                const cgltf_primitive& source = node.mesh->primitives[p];
                if (source.type != cgltf_primitive_type_triangles || source.indices == nullptr)
                {
                    continue;
                }

                const std::size_t firstVertex = model.vertices.size();
                const std::size_t firstIndex = model.indices.size();

                cgltf_size vertexCount = 0;
                bool hasNormals = false;
                bool hasTangents = false;

                for (cgltf_size a = 0; a < source.attributes_count; ++a)
                {
                    const cgltf_attribute& attribute = source.attributes[a];
                    const cgltf_size count = attribute.data->count;
                    vertexCount = std::max(vertexCount, count);
                    if (model.vertices.size() < firstVertex + count)
                    {
                        model.vertices.resize(firstVertex + count);
                    }

                    for (cgltf_size v = 0; v < count; ++v)
                    {
                        Vertex& vertex = model.vertices[firstVertex + v];
                        float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        cgltf_accessor_read_float(attribute.data, v, values, 4);

                        switch (attribute.type)
                        {
                            case cgltf_attribute_type_position:
                            {
                                const Vec4 world =
                                    transform * Vec4{values[0], values[1], values[2], 1.0f};
                                vertex.position = Vec3{world};
                                expandBounds(model.bounds, vertex.position, firstPoint);
                                break;
                            }
                            case cgltf_attribute_type_normal:
                                vertex.normal = glm::normalize(Vec3{
                                    normalMatrix * Vec4{values[0], values[1], values[2], 0.0f}});
                                hasNormals = true;
                                break;
                            case cgltf_attribute_type_tangent:
                            {
                                const Vec3 tangent = Vec3{
                                    transform * Vec4{values[0], values[1], values[2], 0.0f}};
                                vertex.tangent = Vec4{glm::normalize(tangent), values[3]};
                                hasTangents = true;
                                break;
                            }
                            case cgltf_attribute_type_texcoord:
                                if (attribute.index == 0)
                                {
                                    vertex.uv = Vec2{values[0], values[1]};
                                }
                                break;
                            default:
                                break;
                        }
                    }
                }

                const cgltf_size indexCount = source.indices->count;
                model.indices.resize(firstIndex + indexCount);
                for (cgltf_size v = 0; v < indexCount; ++v)
                {
                    model.indices[firstIndex + v] =
                        static_cast<std::uint32_t>(cgltf_accessor_read_index(source.indices, v));
                }

                if (!hasNormals && vertexCount > 0)
                {
                    generateNormals(model, firstVertex, firstIndex, indexCount);
                }
                if (!hasTangents)
                {
                    // Enough for lighting without normal maps.
                    for (cgltf_size v = 0; v < vertexCount; ++v)
                    {
                        Vertex& vertex = model.vertices[firstVertex + v];
                        const Vec3 helper = std::abs(vertex.normal.y) < 0.99f
                                                ? Vec3{0.0f, 1.0f, 0.0f}
                                                : Vec3{1.0f, 0.0f, 0.0f};
                        vertex.tangent =
                            Vec4{glm::normalize(glm::cross(helper, vertex.normal)), 1.0f};
                    }
                }

                Primitive primitive;
                primitive.firstIndex = static_cast<std::uint32_t>(firstIndex);
                primitive.indexCount = static_cast<std::uint32_t>(indexCount);
                primitive.vertexOffset = static_cast<std::int32_t>(firstVertex);
                primitive.materialIndex = 0;
                if (source.material != nullptr)
                {
                    const cgltf_size index =
                        static_cast<cgltf_size>(source.material - data->materials);
                    primitive.materialIndex = static_cast<std::uint32_t>(index + 1);
                }
                model.primitives.push_back(primitive);
            }
        }

        cgltf_free(data);

        if (model.primitives.empty())
        {
            return makeError(ErrorCode::IoFailure,
                             std::format("'{}' contains no triangle geometry", pathText));
        }

        R4D_INFO("glTF: {} vertices, {} triangles, {} primitives, {} materials",
                 model.vertices.size(), model.triangleCount(), model.primitives.size(),
                 model.materials.size());
        return model;
    }

} // namespace r4d
