// GaussianPlyLoader.cpp
//
// Binary little-endian .ply as written by the 3D Gaussian Splatting reference
// implementation. Properties are resolved by name and read through their
// declared type, because exporters disagree on both field order and whether the
// unused normals are stored at all.
#include <radiance4d/scene/GaussianCloud.hpp>

#include <radiance4d/core/Log.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace r4d
{
    namespace
    {
        static_assert(std::endian::native == std::endian::little,
                      "the body is read by memcpy, so the host must be little-endian");

        enum class PlyType
        {
            Int8,
            UInt8,
            Int16,
            UInt16,
            Int32,
            UInt32,
            Float32,
            Float64,
        };

        std::size_t byteSize(PlyType type)
        {
            switch (type)
            {
                case PlyType::Int8:
                case PlyType::UInt8:    return 1;
                case PlyType::Int16:
                case PlyType::UInt16:   return 2;
                case PlyType::Int32:
                case PlyType::UInt32:
                case PlyType::Float32:  return 4;
                case PlyType::Float64:  return 8;
            }
            return 0;
        }

        bool parsePlyType(std::string_view text, PlyType& out)
        {
            // Both the short and the sized spellings appear in the wild.
            if (text == "float" || text == "float32")   { out = PlyType::Float32; return true; }
            if (text == "double" || text == "float64")  { out = PlyType::Float64; return true; }
            if (text == "char" || text == "int8")       { out = PlyType::Int8;    return true; }
            if (text == "uchar" || text == "uint8")     { out = PlyType::UInt8;   return true; }
            if (text == "short" || text == "int16")     { out = PlyType::Int16;   return true; }
            if (text == "ushort" || text == "uint16")   { out = PlyType::UInt16;  return true; }
            if (text == "int" || text == "int32")       { out = PlyType::Int32;   return true; }
            if (text == "uint" || text == "uint32")     { out = PlyType::UInt32;  return true; }
            return false;
        }

        struct PlyProperty
        {
            PlyType type = PlyType::Float32;
            std::size_t offset = 0;
        };

        float readField(const std::byte* vertex, const PlyProperty& property)
        {
            const std::byte* source = vertex + property.offset;
            switch (property.type)
            {
                case PlyType::Float32:
                {
                    float value = 0.0f;
                    std::memcpy(&value, source, sizeof(value));
                    return value;
                }
                case PlyType::Float64:
                {
                    double value = 0.0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
                case PlyType::Int8:
                {
                    std::int8_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
                case PlyType::UInt8:
                {
                    std::uint8_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
                case PlyType::Int16:
                {
                    std::int16_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
                case PlyType::UInt16:
                {
                    std::uint16_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
                case PlyType::Int32:
                {
                    std::int32_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
                case PlyType::UInt32:
                {
                    std::uint32_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    return static_cast<float>(value);
                }
            }
            return 0.0f;
        }

        std::vector<std::string> tokenise(const std::string& line)
        {
            std::vector<std::string> tokens;
            std::size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
                const std::size_t start = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
                if (i > start)
                {
                    tokens.emplace_back(line, start, i - start);
                }
            }
            return tokens;
        }

        // One line at a time, because the binary body starts immediately after
        // end_header and must not be swallowed by a buffered read.
        bool readHeaderLine(std::istream& stream, std::string& line)
        {
            line.clear();
            for (int c = stream.get(); c != std::char_traits<char>::eof(); c = stream.get())
            {
                if (c == '\n')
                {
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    return true;
                }
                line.push_back(static_cast<char>(c));
                if (line.size() > 1024)
                {
                    return false;   // not a .ply header
                }
            }
            return false;
        }

        struct PlyHeader
        {
            std::unordered_map<std::string, PlyProperty> properties;
            std::size_t vertexCount = 0;
            std::size_t stride = 0;
        };

        Result<PlyHeader> parseHeader(std::istream& stream, const std::string& pathText)
        {
            PlyHeader header;
            std::string line;

            if (!readHeaderLine(stream, line) || line != "ply")
            {
                return makeError(ErrorCode::InvalidArgument,
                                 std::format("'{}' does not start with a ply magic line",
                                             pathText));
            }

            bool haveFormat = false;
            bool inVertexElement = false;
            bool haveVertexElement = false;

            while (true)
            {
                if (!readHeaderLine(stream, line))
                {
                    return makeError(ErrorCode::InvalidArgument,
                                     std::format("'{}' ends before end_header", pathText));
                }

                const std::vector<std::string> tokens = tokenise(line);
                if (tokens.empty() || tokens[0] == "comment" || tokens[0] == "obj_info")
                {
                    continue;
                }

                if (tokens[0] == "end_header")
                {
                    break;
                }

                if (tokens[0] == "format")
                {
                    if (tokens.size() < 2)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' has a malformed format line",
                                                     pathText));
                    }
                    if (tokens[1] != "binary_little_endian")
                    {
                        return makeError(
                            ErrorCode::UnsupportedFeature,
                            std::format("'{}' is ply format '{}'; only "
                                        "binary_little_endian is supported",
                                        pathText, tokens[1]));
                    }
                    haveFormat = true;
                    continue;
                }

                if (tokens[0] == "element")
                {
                    if (tokens.size() < 3)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' has a malformed element line",
                                                     pathText));
                    }
                    inVertexElement = tokens[1] == "vertex";
                    if (!inVertexElement)
                    {
                        // Splat datasets carry nothing but vertices, and guessing
                        // where a second element's body starts is how offsets end
                        // up silently wrong.
                        return makeError(
                            ErrorCode::UnsupportedFeature,
                            std::format("'{}' declares element '{}'; only a single "
                                        "vertex element is supported",
                                        pathText, tokens[1]));
                    }
                    if (haveVertexElement)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' declares vertex twice", pathText));
                    }
                    haveVertexElement = true;

                    unsigned long long count = 0;
                    try
                    {
                        count = std::stoull(tokens[2]);
                    }
                    catch (const std::exception&)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' has a non-numeric vertex count '{}'",
                                                     pathText, tokens[2]));
                    }
                    header.vertexCount = static_cast<std::size_t>(count);
                    continue;
                }

                if (tokens[0] == "property")
                {
                    if (!inVertexElement)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' has a property outside any element",
                                                     pathText));
                    }
                    if (tokens.size() < 3)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' has a malformed property line",
                                                     pathText));
                    }
                    if (tokens[1] == "list")
                    {
                        return makeError(
                            ErrorCode::UnsupportedFeature,
                            std::format("'{}' has a list property on the vertex element, "
                                        "which makes the stride variable",
                                        pathText));
                    }

                    PlyProperty property;
                    if (!parsePlyType(tokens[1], property.type))
                    {
                        return makeError(ErrorCode::UnsupportedFeature,
                                         std::format("'{}' uses unknown property type '{}'",
                                                     pathText, tokens[1]));
                    }
                    property.offset = header.stride;
                    header.stride += byteSize(property.type);

                    if (!header.properties.emplace(tokens[2], property).second)
                    {
                        return makeError(ErrorCode::InvalidArgument,
                                         std::format("'{}' declares property '{}' twice",
                                                     pathText, tokens[2]));
                    }
                    continue;
                }

                return makeError(ErrorCode::InvalidArgument,
                                 std::format("'{}' has an unexpected header line '{}'",
                                             pathText, line));
            }

            if (!haveFormat)
            {
                return makeError(ErrorCode::InvalidArgument,
                                 std::format("'{}' has no format line", pathText));
            }
            if (!haveVertexElement)
            {
                return makeError(ErrorCode::InvalidArgument,
                                 std::format("'{}' has no vertex element", pathText));
            }
            return header;
        }

        // The destinations a vertex is scattered into, resolved once.
        struct Fields
        {
            PlyProperty position[3];
            PlyProperty scale[3];
            PlyProperty rotation[4];
            PlyProperty opacity;
            PlyProperty dc[3];
            std::vector<PlyProperty> rest;   // channel-major, as the file stores it
            int degree = 0;
            int coefficients = 1;
        };

        Result<Fields> resolveFields(const PlyHeader& header, const std::string& pathText)
        {
            Fields fields;

            const auto require = [&](const std::string& name,
                                     PlyProperty& out) -> Status {
                const auto found = header.properties.find(name);
                if (found == header.properties.end())
                {
                    return makeError(ErrorCode::InvalidArgument,
                                     std::format("'{}' has no '{}' property", pathText, name));
                }
                out = found->second;
                return Status::Ok();
            };

            R4D_TRY(require("x", fields.position[0]));
            R4D_TRY(require("y", fields.position[1]));
            R4D_TRY(require("z", fields.position[2]));
            R4D_TRY(require("opacity", fields.opacity));
            for (int i = 0; i < 3; ++i)
            {
                R4D_TRY(require(std::format("scale_{}", i), fields.scale[i]));
                R4D_TRY(require(std::format("f_dc_{}", i), fields.dc[i]));
            }
            for (int i = 0; i < 4; ++i)
            {
                R4D_TRY(require(std::format("rot_{}", i), fields.rotation[i]));
            }

            // f_rest must run from 0 without gaps, or the band a coefficient
            // belongs to cannot be worked out from its index.
            std::size_t contiguous = 0;
            while (header.properties.contains(std::format("f_rest_{}", contiguous)))
            {
                ++contiguous;
            }
            std::size_t declared = 0;
            for (const auto& entry : header.properties)
            {
                if (entry.first.starts_with("f_rest_"))
                {
                    ++declared;
                }
            }
            if (declared != contiguous)
            {
                return makeError(ErrorCode::InvalidArgument,
                                 std::format("'{}' has {} f_rest properties but only {} "
                                             "run contiguously from f_rest_0",
                                             pathText, declared, contiguous));
            }

            switch (contiguous)
            {
                case 0:  fields.degree = 0; break;
                case 9:  fields.degree = 1; break;
                case 24: fields.degree = 2; break;
                case 45: fields.degree = 3; break;
                default:
                    return makeError(
                        ErrorCode::UnsupportedFeature,
                        std::format("'{}' has {} f_rest properties, which is not a "
                                    "spherical-harmonic degree of 0, 1, 2 or 3",
                                    pathText, contiguous));
            }
            fields.coefficients = (fields.degree + 1) * (fields.degree + 1);

            fields.rest.resize(contiguous);
            for (std::size_t i = 0; i < contiguous; ++i)
            {
                fields.rest[i] = header.properties.at(std::format("f_rest_{}", i));
            }
            return fields;
        }

        // Bands whose basis polynomial is odd in y, in the Splat.vs.hlsl order:
        // 1 is y, 4 is xy, 5 is yz, 9 is y(3x^2 - y^2), 10 is xyz and 11 is
        // y(4z^2 - x^2 - y^2). The other ten are even in y.
        constexpr int kYOddBands[] = {1, 4, 5, 9, 10, 11};

        // COLMAP's world frame, which 3DGS trains and publishes in, is
        // right-handed with +y down; the engine is GLM_FORCE_LEFT_HANDED with +y
        // up. The two differ by the reflection M = diag(1, -1, 1), applied here
        // so a dataset arrives the way its publisher renders it rather than
        // upside down.
        //
        // The covariance transforms as M (R S S^T R^T) M^T, which factors into
        // (M R M)(S S^T)(M R M)^T because M is its own inverse and S S^T is
        // diagonal: the scales are untouched and the rotation becomes M R M,
        // still a rotation since det(M)^2 = 1. Conjugating a rotation by an
        // orthogonal Q maps its axis to det(Q) Q a and leaves the angle alone,
        // so with det(M) = -1 the axis (x, y, z) becomes (-x, y, -z) and the
        // quaternion (w, x, y, z) becomes (w, -x, y, -z).
        //
        // The SH basis is evaluated from the view direction, whose y flips along
        // with everything else, so every coefficient on a basis function that is
        // odd in y has to change sign for the colour to come out unchanged.
        void mirrorIntoEngineFrame(Gaussian& g)
        {
            g.position.y = -g.position.y;
            g.rotation = Quat{g.rotation.w, -g.rotation.x, g.rotation.y, -g.rotation.z};

            for (const int band : kYOddBands)
            {
                if (band >= kShCoefficients)
                {
                    break;      // a lower runtime degree simply has fewer of them
                }
                for (int c = 0; c < 3; ++c)
                {
                    g.sh[band * 3 + c] = -g.sh[band * 3 + c];
                }
            }
        }

        Gaussian convertVertex(const std::byte* vertex, const Fields& fields)
        {
            Gaussian g;
            g.position = Vec3{readField(vertex, fields.position[0]),
                              readField(vertex, fields.position[1]),
                              readField(vertex, fields.position[2])};

            // Opacity is stored pre-activation, scales as logarithms.
            g.opacity = 1.0f / (1.0f + std::exp(-readField(vertex, fields.opacity)));
            g.scale = Vec3{std::exp(readField(vertex, fields.scale[0])),
                           std::exp(readField(vertex, fields.scale[1])),
                           std::exp(readField(vertex, fields.scale[2]))};

            // rot_0..3 are wxyz, which is also the order glm's scalar constructor
            // takes; the storage order underneath is xyzw.
            const Quat raw{readField(vertex, fields.rotation[0]),
                           readField(vertex, fields.rotation[1]),
                           readField(vertex, fields.rotation[2]),
                           readField(vertex, fields.rotation[3])};
            const float length = glm::length(raw);
            g.rotation = length > 1e-12f ? raw / length : Quat{1.0f, 0.0f, 0.0f, 0.0f};

            for (int c = 0; c < 3; ++c)
            {
                g.sh[c] = readField(vertex, fields.dc[c]);
            }
            // The file is channel-major and the POD coefficient-major, so this is
            // a transpose. Bands the file does not carry stay zero.
            const int perChannel = fields.coefficients - 1;
            for (int c = 0; c < 3; ++c)
            {
                for (int k = 0; k < perChannel; ++k)
                {
                    const auto source = static_cast<std::size_t>(c * perChannel + k);
                    g.sh[(k + 1) * 3 + c] = readField(vertex, fields.rest[source]);
                }
            }

            mirrorIntoEngineFrame(g);
            return g;
        }
    } // namespace

    Result<GaussianCloud> loadGaussianPly(const std::filesystem::path& path)
    {
        const std::string pathText = path.string();

        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            return makeError(ErrorCode::NotFound,
                             std::format("cannot open '{}'", pathText));
        }

        auto header = parseHeader(file, pathText);
        if (!header)
        {
            return header.error();
        }
        auto fields = resolveFields(header.value(), pathText);
        if (!fields)
        {
            return fields.error();
        }

        const std::size_t count = header.value().vertexCount;
        const std::size_t stride = header.value().stride;

        GaussianCloud cloud;
        cloud.gaussians.reserve(count);

        // Read in batches: a million degree-3 splats is a quarter of a gigabyte
        // of body, and holding the file and the cloud at once is avoidable.
        constexpr std::size_t kBatchVertices = 4096;
        std::vector<std::byte> buffer(kBatchVertices * stride);
        for (std::size_t done = 0; done < count;)
        {
            const std::size_t batch = std::min(kBatchVertices, count - done);
            const std::streamsize wanted = static_cast<std::streamsize>(batch * stride);
            file.read(reinterpret_cast<char*>(buffer.data()), wanted);
            if (file.gcount() != wanted)
            {
                return makeError(
                    ErrorCode::InvalidArgument,
                    std::format("'{}' is truncated: the header declares {} vertices of "
                                "{} bytes, the body ends after {}",
                                pathText, count, stride,
                                done + static_cast<std::size_t>(file.gcount()) / stride));
            }
            for (std::size_t i = 0; i < batch; ++i)
            {
                cloud.gaussians.push_back(
                    convertVertex(buffer.data() + i * stride, fields.value()));
            }
            done += batch;
        }

        cloud.recomputeBounds();

        R4D_INFO("Gaussians: {} splats from '{}', SH degree {} in the file ({:.1f} MiB "
                 "on the GPU)",
                 cloud.count(), pathText, fields.value().degree,
                 static_cast<double>(cloud.count() * sizeof(Gaussian)) / (1024.0 * 1024.0));
        if (fields.value().degree != kShDegree)
        {
            R4D_WARN("Gaussians: the file carries SH degree {} and the runtime degree {}; "
                     "the missing bands are zero",
                     fields.value().degree, kShDegree);
        }
        return cloud;
    }

} // namespace r4d
