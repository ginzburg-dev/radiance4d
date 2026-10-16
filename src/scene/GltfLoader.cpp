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
#include <cstdlib>
#include <map>
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

        // Resolves a glTF image to pixels: buffer view, data URI or a file
        // beside the document.
        std::int32_t loadTexture(Model& model, const cgltf_texture* texture,
                                 const std::filesystem::path& baseDirectory, bool srgb,
                                 std::map<const cgltf_image*, std::int32_t>& cache)
        {
            if (texture == nullptr || texture->image == nullptr)
            {
                return -1;
            }
            const cgltf_image* image = texture->image;
            if (const auto found = cache.find(image); found != cache.end())
            {
                return found->second;
            }

            Result<ImageRgba8> decoded = makeError(ErrorCode::NotFound, "no image source");

            if (image->buffer_view != nullptr && image->buffer_view->buffer != nullptr &&
                image->buffer_view->buffer->data != nullptr)
            {
                const auto* bytes = static_cast<const std::byte*>(
                    image->buffer_view->buffer->data);
                decoded = decodeImage(bytes + image->buffer_view->offset,
                                      image->buffer_view->size);
            }
            else if (image->uri != nullptr)
            {
                const std::string uri = image->uri;
                if (uri.rfind("data:", 0) == 0)
                {
                    const std::size_t comma = uri.find(',');
                    if (comma != std::string::npos)
                    {
                        void* raw = nullptr;
                        cgltf_options options{};
                        const std::string payload = uri.substr(comma + 1);
                        // Decoded size is at most 3/4 of the base64 text.
                        const cgltf_size size = payload.size() / 4 * 3;
                        if (cgltf_load_buffer_base64(&options, size, payload.c_str(), &raw) ==
                                cgltf_result_success &&
                            raw != nullptr)
                        {
                            decoded = decodeImage(static_cast<const std::byte*>(raw), size);
                            free(raw);
                        }
                    }
                }
                else
                {
                    decoded = readPng(baseDirectory / uri);
                }
            }

            if (!decoded)
            {
                R4D_WARN("glTF: cannot load texture: {}", decoded.error().message);
                cache.emplace(image, -1);
                return -1;
            }

            const auto index = static_cast<std::int32_t>(model.textures.size());
            model.textures.push_back(TextureImage{std::move(decoded.value()), srgb});
            cache.emplace(image, index);
            return index;
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

        Material fallback;
        fallback.name = "default";
        fallback.baseColour = Vec4{0.8f, 0.8f, 0.8f, 1.0f};
        fallback.metallic = 0.0f;
        fallback.roughness = 0.6f;
        model.materials.push_back(fallback);

        const std::filesystem::path baseDirectory = path.parent_path();
        std::map<const cgltf_image*, std::int32_t> textureCache;

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
            if (source.has_pbr_metallic_roughness)
            {
                const auto& pbr = source.pbr_metallic_roughness;
                material.baseColourTexture = loadTexture(
                    model, pbr.base_color_texture.texture, baseDirectory, true, textureCache);
                material.metallicRoughnessTexture =
                    loadTexture(model, pbr.metallic_roughness_texture.texture, baseDirectory,
                                false, textureCache);
            }
            material.normalTexture = loadTexture(model, source.normal_texture.texture,
                                                 baseDirectory, false, textureCache);
            material.emissive = Vec3{source.emissive_factor[0], source.emissive_factor[1],
                                     source.emissive_factor[2]};
            // The factor is a multiplier on the texture, not a value of its own:
            // DamagedHelmet sets it to white and masks the glow with the map.
            material.emissiveTexture = loadTexture(model, source.emissive_texture.texture,
                                                   baseDirectory, true, textureCache);
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

        for (std::size_t i = 0; i < model.materials.size(); ++i)
        {
            const Material& m = model.materials[i];
            R4D_INFO("glTF material[{}] {}: base({:.2f},{:.2f},{:.2f}) metal {:.2f} rough {:.2f} "
                     "tex base={} mr={} nrm={}",
                     i, m.name, m.baseColour.x, m.baseColour.y, m.baseColour.z, m.metallic,
                     m.roughness, m.baseColourTexture, m.metallicRoughnessTexture,
                     m.normalTexture);
        }
        {
            Vec2 uvMin{1e9f, 1e9f};
            Vec2 uvMax{-1e9f, -1e9f};
            for (const Vertex& v : model.vertices)
            {
                uvMin = glm::min(uvMin, v.uv);
                uvMax = glm::max(uvMax, v.uv);
            }
            R4D_INFO("glTF uv range: ({:.3f},{:.3f}) .. ({:.3f},{:.3f})", uvMin.x, uvMin.y,
                     uvMax.x, uvMax.y);
            R4D_INFO("glTF bounds: ({:.2f},{:.2f},{:.2f}) .. ({:.2f},{:.2f},{:.2f})",
                     model.bounds.min.x, model.bounds.min.y, model.bounds.min.z,
                     model.bounds.max.x, model.bounds.max.y, model.bounds.max.z);
        }
        for (std::size_t i = 0; i < model.textures.size(); ++i)
        {
            R4D_INFO("glTF texture[{}]: {}x{} srgb {}", i, model.textures[i].image.width,
                     model.textures[i].image.height, model.textures[i].srgb);
        }

        attachDefaultEnvironment(model);

        R4D_INFO("glTF: {} vertices, {} triangles, {} primitives, {} materials, {} textures",
                 model.vertices.size(), model.triangleCount(), model.primitives.size(),
                 model.materials.size(), model.textures.size());
        return model;
    }

} // namespace r4d
