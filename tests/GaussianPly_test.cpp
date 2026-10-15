// GaussianPly_test.cpp
//
// Published datasets are hundreds of megabytes to several gigabytes and cannot
// live in the repository, so the suite writes its own .ply and asserts the
// conversions numerically: exponentiated log-scales, a normalised quaternion in
// the right component order, the opacity sigmoid, the SH transpose and the
// mirror out of COLMAP's y-down frame. Loading a published scene stays a manual
// step, written down in the README.

#include <radiance4d/scene/GaussianCloud.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    struct Field
    {
        std::string name;
        std::string type = "float";
        double value = 0.0;
    };

    using Vertex = std::vector<Field>;

    void appendScalar(std::string& body, const Field& field)
    {
        const auto append = [&body](const void* bytes, std::size_t size) {
            body.append(static_cast<const char*>(bytes), size);
        };
        if (field.type == "float")
        {
            const auto value = static_cast<float>(field.value);
            append(&value, sizeof(value));
        }
        else if (field.type == "double")
        {
            append(&field.value, sizeof(field.value));
        }
        else if (field.type == "uchar")
        {
            const auto value = static_cast<std::uint8_t>(field.value);
            append(&value, sizeof(value));
        }
        else if (field.type == "int")
        {
            const auto value = static_cast<std::int32_t>(field.value);
            append(&value, sizeof(value));
        }
        else
        {
            ADD_FAILURE() << "the test writer cannot emit property type " << field.type;
        }
    }

    struct PlyOptions
    {
        std::string format = "binary_little_endian 1.0";
        std::size_t dropTrailingBytes = 0;
        // Appended after the vertex properties, which is where a second element
        // or a list property would appear.
        std::vector<std::string> extraHeaderLines;
    };

    void writePly(const fs::path& path, const std::vector<Vertex>& vertices,
                  const PlyOptions& options = {})
    {
        std::string header = "ply\nformat " + options.format + "\n";
        header += "comment written by GaussianPly_test\n";
        header += "element vertex " + std::to_string(vertices.size()) + "\n";
        if (!vertices.empty())
        {
            for (const Field& field : vertices.front())
            {
                header += "property " + field.type + " " + field.name + "\n";
            }
        }
        for (const std::string& line : options.extraHeaderLines)
        {
            header += line + "\n";
        }
        header += "end_header\n";

        std::string body;
        for (const Vertex& vertex : vertices)
        {
            for (const Field& field : vertex)
            {
                appendScalar(body, field);
            }
        }
        body.resize(options.dropTrailingBytes >= body.size()
                        ? 0
                        : body.size() - options.dropTrailingBytes);

        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(file.good()) << "cannot write " << path.string();
        file.write(header.data(), static_cast<std::streamsize>(header.size()));
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    // Everything a .ply stores before activation.
    struct RawSplat
    {
        double position[3]{1.5, -2.25, 0.75};
        double logScale[3]{-1.3862943611, 0.0, 1.0986122887};   // exp: 0.25, 1, 3
        double rotation[4]{1.0, 2.0, 3.0, 4.0};                 // wxyz, unnormalised
        double opacity = 2.0;                                   // sigmoid: 0.8807971
        double dc[3]{0.4, -0.2, 0.9};
    };

    // Identifies both the channel and the band a coefficient must end up in, so
    // a transposed or shifted SH copy cannot pass.
    double restValue(int channel, int band) { return 100.0 * channel + band + 1.0; }

    // The loader mirrors the file's y, and a basis function odd in y changes
    // sign with it. These six are the odd ones in the Splat.vs.hlsl ordering.
    bool bandIsOddInY(int band)
    {
        return band == 1 || band == 4 || band == 5 || band == 9 || band == 10 || band == 11;
    }

    // What f_rest_(channel * perChannel + band - 1) must read back as.
    double mirroredRestValue(int channel, int band)
    {
        const double value = restValue(channel, band - 1);
        return bandIsOddInY(band) ? -value : value;
    }

    int coefficientsFor(int degree) { return (degree + 1) * (degree + 1); }

    // The basis evaluateSh in Splat.vs.hlsl uses, duplicated so a test can state
    // what the frame change must preserve -- the colour along a direction --
    // instead of restating the signs the loader applies. If the shader's basis
    // ever changes, this has to move with it.
    r4d::Vec3 evaluateSh(const float* sh, r4d::Vec3 d)
    {
        static_assert(r4d::kShCoefficients == 16, "this basis is written out to degree 3");
        const auto band = [sh](int k) {
            return r4d::Vec3{sh[k * 3], sh[k * 3 + 1], sh[k * 3 + 2]};
        };
        const float x = d.x, y = d.y, z = d.z;
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, yz = y * z, xz = x * z;

        r4d::Vec3 colour = 0.28209479177387814f * band(0);
        colour += 0.4886025119029199f * (-y * band(1) + z * band(2) - x * band(3));
        colour += 1.0925484305920792f * xy * band(4);
        colour += -1.0925484305920792f * yz * band(5);
        colour += 0.31539156525252005f * (2.0f * zz - xx - yy) * band(6);
        colour += -1.0925484305920792f * xz * band(7);
        colour += 0.5462742152960396f * (xx - yy) * band(8);
        colour += -0.5900435899266435f * y * (3.0f * xx - yy) * band(9);
        colour += 2.890611442640554f * xy * z * band(10);
        colour += -0.4570457994644658f * y * (4.0f * zz - xx - yy) * band(11);
        colour += 0.3731763325901154f * z * (2.0f * zz - 3.0f * xx - 3.0f * yy) * band(12);
        colour += -0.4570457994644658f * x * (4.0f * zz - xx - yy) * band(13);
        colour += 1.445305721320277f * z * (xx - yy) * band(14);
        colour += -0.5900435899266435f * x * (xx - 3.0f * yy) * band(15);
        return colour;
    }

    // The property order the reference implementation writes.
    Vertex canonicalVertex(const RawSplat& raw, int degree)
    {
        const int perChannel = coefficientsFor(degree) - 1;

        Vertex vertex;
        vertex.push_back({"x", "float", raw.position[0]});
        vertex.push_back({"y", "float", raw.position[1]});
        vertex.push_back({"z", "float", raw.position[2]});
        vertex.push_back({"nx", "float", 0.0});
        vertex.push_back({"ny", "float", 0.0});
        vertex.push_back({"nz", "float", 0.0});
        for (int c = 0; c < 3; ++c)
        {
            vertex.push_back({"f_dc_" + std::to_string(c), "float", raw.dc[c]});
        }
        for (int c = 0; c < 3; ++c)
        {
            for (int k = 0; k < perChannel; ++k)
            {
                vertex.push_back({"f_rest_" + std::to_string(c * perChannel + k), "float",
                                  restValue(c, k)});
            }
        }
        vertex.push_back({"opacity", "float", raw.opacity});
        for (int i = 0; i < 3; ++i)
        {
            vertex.push_back({"scale_" + std::to_string(i), "float", raw.logScale[i]});
        }
        for (int i = 0; i < 4; ++i)
        {
            vertex.push_back({"rot_" + std::to_string(i), "float", raw.rotation[i]});
        }
        return vertex;
    }

    Vertex withoutProperty(Vertex vertex, const std::string& name)
    {
        std::erase_if(vertex, [&name](const Field& field) { return field.name == name; });
        return vertex;
    }

    class GaussianPly : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_directory = fs::temp_directory_path() /
                          ("r4d_gaussian_ply_" + std::string{info->name()});
            std::error_code ignored;
            fs::remove_all(m_directory, ignored);
            ASSERT_TRUE(fs::create_directories(m_directory));
        }

        void TearDown() override
        {
            std::error_code ignored;
            fs::remove_all(m_directory, ignored);
        }

        fs::path path(const std::string& name = "cloud.ply") const
        {
            return m_directory / name;
        }

    private:
        fs::path m_directory;
    };

    constexpr float kTolerance = 1e-5f;
} // namespace

TEST_F(GaussianPly, ConvertsTheStoredActivationsOnLoad)
{
    const RawSplat raw;
    writePly(path(), {canonicalVertex(raw, 3)});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();
    ASSERT_EQ(loaded.value().count(), 1u);
    const r4d::Gaussian& g = loaded.value().gaussians.front();

    EXPECT_NEAR(g.position.x, 1.5f, kTolerance);
    EXPECT_NEAR(g.position.y, 2.25f, kTolerance);   // mirrored out of COLMAP's frame
    EXPECT_NEAR(g.position.z, 0.75f, kTolerance);

    // Log-scales are exponentiated, not passed through.
    EXPECT_NEAR(g.scale.x, 0.25f, kTolerance);
    EXPECT_NEAR(g.scale.y, 1.0f, kTolerance);
    EXPECT_NEAR(g.scale.z, 3.0f, kTolerance);

    // Opacity goes through a sigmoid: 1 / (1 + exp(-2)).
    EXPECT_NEAR(g.opacity, 0.8807970779f, kTolerance);

    EXPECT_TRUE(loaded.value().bounds.valid());
}

TEST_F(GaussianPly, QuaternionIsNormalisedAndReadAsWxyz)
{
    // The single easiest bug to ship here: .ply stores wxyz, the POD stores xyzw.
    // Four distinct components pin both the order and the normalisation; x and z
    // then come back negated because the y mirror below turns the axis around.
    const RawSplat raw;
    writePly(path(), {canonicalVertex(raw, 3)});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();
    const r4d::Quat& q = loaded.value().gaussians.front().rotation;

    const float norm = std::sqrt(1.0f + 4.0f + 9.0f + 16.0f);   // sqrt(30)
    EXPECT_NEAR(q.w, 1.0f / norm, kTolerance);
    EXPECT_NEAR(q.x, -2.0f / norm, kTolerance);
    EXPECT_NEAR(q.y, 3.0f / norm, kTolerance);
    EXPECT_NEAR(q.z, -4.0f / norm, kTolerance);
    EXPECT_NEAR(glm::length(q), 1.0f, kTolerance);
}

TEST_F(GaussianPly, TransposesTheChannelMajorShIntoCoefficientMajor)
{
    const RawSplat raw;
    writePly(path(), {canonicalVertex(raw, 3)});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();
    const r4d::Gaussian& g = loaded.value().gaussians.front();

    // f_dc_0..2 are the degree-0 band, one per channel.
    EXPECT_NEAR(g.sh[0], 0.4f, kTolerance);
    EXPECT_NEAR(g.sh[1], -0.2f, kTolerance);
    EXPECT_NEAR(g.sh[2], 0.9f, kTolerance);

    // Mid grey plus the degree-0 offset is the colour a degree-0 cloud shows.
    const r4d::Vec3 constant = r4d::constantColour(g);
    EXPECT_NEAR(constant.x, r4d::kShC0 * 0.4f + 0.5f, kTolerance);
    EXPECT_NEAR(constant.y, r4d::kShC0 * -0.2f + 0.5f, kTolerance);
    EXPECT_NEAR(constant.z, r4d::kShC0 * 0.9f + 0.5f, kTolerance);

    for (int band = 1; band < r4d::kShCoefficients; ++band)
    {
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(g.sh[band * 3 + c], static_cast<float>(mirroredRestValue(c, band)),
                        kTolerance)
                << "band " << band << " channel " << c;
        }
    }
}

TEST_F(GaussianPly, MirrorsTheRotationSoTheCovarianceIsReflected)
{
    // (w, -x, y, -z) is only shorthand. What the loader has to deliver is the
    // rotation of the reflected covariance, M R S S^T R^T M^T with
    // M = diag(1, -1, 1), which is R' = M R M at unchanged scales.
    const RawSplat raw;
    writePly(path(), {canonicalVertex(raw, 3)});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();
    const r4d::Gaussian& g = loaded.value().gaussians.front();

    const r4d::Quat fileRotation = glm::normalize(
        r4d::Quat{static_cast<float>(raw.rotation[0]), static_cast<float>(raw.rotation[1]),
                  static_cast<float>(raw.rotation[2]), static_cast<float>(raw.rotation[3])});
    const r4d::Mat3 mirror{1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const r4d::Mat3 want = mirror * glm::mat3_cast(fileRotation) * mirror;
    const r4d::Mat3 got = glm::mat3_cast(g.rotation);

    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            EXPECT_NEAR(got[column][row], want[column][row], kTolerance)
                << "column " << column << " row " << row;
        }
    }

    // A reflection would flip the handedness of the splat's own frame.
    EXPECT_NEAR(glm::determinant(got), 1.0f, kTolerance);
    EXPECT_NEAR(g.scale.x, 0.25f, kTolerance);
    EXPECT_NEAR(g.scale.y, 1.0f, kTolerance);
    EXPECT_NEAR(g.scale.z, 3.0f, kTolerance);
}

TEST_F(GaussianPly, MirroredShShowsTheSameColourAlongTheMirroredDirection)
{
    // The property the SH sign flips exist for: a splat seen along d in the
    // file's frame must be the colour seen along the mirrored d in ours. That
    // is stronger than restating which six bands the loader negates.
    const RawSplat raw;
    writePly(path(), {canonicalVertex(raw, 3)});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();

    float fileSh[r4d::kShCoefficients * 3]{};
    for (int c = 0; c < 3; ++c)
    {
        fileSh[c] = static_cast<float>(raw.dc[c]);
        for (int band = 1; band < r4d::kShCoefficients; ++band)
        {
            fileSh[band * 3 + c] = static_cast<float>(restValue(c, band - 1));
        }
    }

    // Deliberately off-axis, so no basis function sits at a zero of its own.
    const r4d::Vec3 directions[]{{0.3f, 0.5f, -0.81f}, {-0.6f, -0.2f, 0.77f},
                                 {0.48f, -0.73f, 0.49f}, {-0.35f, 0.62f, 0.70f}};
    for (const r4d::Vec3& raw3 : directions)
    {
        const r4d::Vec3 d = glm::normalize(raw3);
        const r4d::Vec3 want = evaluateSh(fileSh, d);
        const r4d::Vec3 got =
            evaluateSh(loaded.value().gaussians.front().sh, r4d::Vec3{d.x, -d.y, d.z});

        // The coefficients run to a few hundred, so this is a relative bound.
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(got[c], want[c], 1e-3f * (1.0f + std::abs(want[c])))
                << "direction (" << d.x << ", " << d.y << ", " << d.z << ") channel " << c;
        }
    }
}

TEST_F(GaussianPly, MatchesPropertiesByNameNotByOffset)
{
    // Exporters disagree on field order, and extra properties are common.
    const RawSplat raw;
    Vertex shuffled = canonicalVertex(raw, 3);
    std::reverse(shuffled.begin(), shuffled.end());
    shuffled.insert(shuffled.begin(), Field{"red", "uchar", 200.0});
    shuffled.push_back(Field{"r4d_unused", "int", -7.0});

    writePly(path("canonical.ply"), {canonicalVertex(raw, 3)});
    writePly(path("shuffled.ply"), {shuffled});

    auto canonical = r4d::loadGaussianPly(path("canonical.ply"));
    ASSERT_TRUE(canonical.ok()) << canonical.error().ToString();
    auto reordered = r4d::loadGaussianPly(path("shuffled.ply"));
    ASSERT_TRUE(reordered.ok()) << reordered.error().ToString();

    ASSERT_EQ(reordered.value().count(), 1u);
    EXPECT_EQ(std::memcmp(&canonical.value().gaussians.front(),
                          &reordered.value().gaussians.front(), sizeof(r4d::Gaussian)),
              0);
}

TEST_F(GaussianPly, ReadsPropertiesThroughTheirDeclaredType)
{
    const RawSplat raw;
    Vertex widened = canonicalVertex(raw, 3);
    for (Field& field : widened)
    {
        if (field.name == "x" || field.name == "opacity" || field.name == "rot_0")
        {
            field.type = "double";
        }
    }

    writePly(path("float.ply"), {canonicalVertex(raw, 3)});
    writePly(path("double.ply"), {widened});

    auto narrow = r4d::loadGaussianPly(path("float.ply"));
    ASSERT_TRUE(narrow.ok()) << narrow.error().ToString();
    auto wide = r4d::loadGaussianPly(path("double.ply"));
    ASSERT_TRUE(wide.ok()) << wide.error().ToString();

    const r4d::Gaussian& a = narrow.value().gaussians.front();
    const r4d::Gaussian& b = wide.value().gaussians.front();
    EXPECT_NEAR(a.position.x, b.position.x, kTolerance);
    EXPECT_NEAR(a.opacity, b.opacity, kTolerance);
    EXPECT_NEAR(a.rotation.w, b.rotation.w, kTolerance);
}

TEST_F(GaussianPly, ALowerDegreeFileLeavesTheMissingBandsZero)
{
    const RawSplat raw;
    writePly(path("degree0.ply"), {canonicalVertex(raw, 0)});
    writePly(path("degree1.ply"), {canonicalVertex(raw, 1)});

    auto flat = r4d::loadGaussianPly(path("degree0.ply"));
    ASSERT_TRUE(flat.ok()) << flat.error().ToString();
    for (int i = 3; i < r4d::kShCoefficients * 3; ++i)
    {
        EXPECT_EQ(flat.value().gaussians.front().sh[i], 0.0f) << "slot " << i;
    }

    auto linear = r4d::loadGaussianPly(path("degree1.ply"));
    ASSERT_TRUE(linear.ok()) << linear.error().ToString();
    const r4d::Gaussian& g = linear.value().gaussians.front();
    for (int band = 1; band < 4; ++band)
    {
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(g.sh[band * 3 + c], static_cast<float>(mirroredRestValue(c, band)),
                        kTolerance);
        }
    }
    for (int i = 4 * 3; i < r4d::kShCoefficients * 3; ++i)
    {
        EXPECT_EQ(g.sh[i], 0.0f) << "slot " << i;
    }
}

TEST_F(GaussianPly, ReadsMoreSplatsThanOneReadBatch)
{
    // The body is read in batches, so the arithmetic has to survive a boundary.
    const RawSplat raw;
    constexpr std::size_t kCount = 5000;
    std::vector<Vertex> vertices;
    vertices.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i)
    {
        RawSplat moved = raw;
        moved.position[0] = static_cast<double>(i);
        vertices.push_back(canonicalVertex(moved, 3));
    }
    writePly(path(), vertices);

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();
    ASSERT_EQ(loaded.value().count(), kCount);
    EXPECT_NEAR(loaded.value().gaussians.front().position.x, 0.0f, kTolerance);
    EXPECT_NEAR(loaded.value().gaussians[4095].position.x, 4095.0f, kTolerance);
    EXPECT_NEAR(loaded.value().gaussians[4096].position.x, 4096.0f, kTolerance);
    EXPECT_NEAR(loaded.value().gaussians.back().position.x,
                static_cast<float>(kCount - 1), kTolerance);
}

TEST_F(GaussianPly, AnEmptyCloudLoads)
{
    const RawSplat raw;
    // The writer takes its property list from a vertex, so a zero-vertex file
    // has to be declared by hand.
    const Vertex declaration = canonicalVertex(raw, 3);
    {
        std::string header = "ply\nformat binary_little_endian 1.0\nelement vertex 0\n";
        for (const Field& field : declaration)
        {
            header += "property " + field.type + " " + field.name + "\n";
        }
        header += "end_header\n";
        std::ofstream file{path(), std::ios::binary | std::ios::trunc};
        file.write(header.data(), static_cast<std::streamsize>(header.size()));
    }

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_TRUE(loaded.ok()) << loaded.error().ToString();
    EXPECT_TRUE(loaded.value().empty());
}

TEST_F(GaussianPly, RejectsAMissingFile)
{
    auto loaded = r4d::loadGaussianPly(path("absent.ply"));
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::NotFound);
}

TEST_F(GaussianPly, RejectsAFileThatIsNotPly)
{
    std::ofstream file{path(), std::ios::binary | std::ios::trunc};
    file << "this is not a point cloud\n";
    file.close();

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::InvalidArgument);
}

TEST_F(GaussianPly, RejectsAsciiFormat)
{
    const RawSplat raw;
    PlyOptions options;
    options.format = "ascii 1.0";
    writePly(path(), {canonicalVertex(raw, 3)}, options);

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::UnsupportedFeature);
    EXPECT_NE(loaded.error().message.find("binary_little_endian"), std::string::npos)
        << loaded.error().message;
}

TEST_F(GaussianPly, RejectsBigEndianFormat)
{
    const RawSplat raw;
    PlyOptions options;
    options.format = "binary_big_endian 1.0";
    writePly(path(), {canonicalVertex(raw, 3)}, options);

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::UnsupportedFeature);
}

TEST_F(GaussianPly, RejectsATruncatedBody)
{
    const RawSplat raw;
    PlyOptions options;
    options.dropTrailingBytes = 17;
    writePly(path(), {canonicalVertex(raw, 3), canonicalVertex(raw, 3)}, options);

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::InvalidArgument);
    EXPECT_NE(loaded.error().message.find("truncated"), std::string::npos)
        << loaded.error().message;
}

TEST_F(GaussianPly, RejectsAMissingRequiredProperty)
{
    const RawSplat raw;
    writePly(path("no_opacity.ply"), {withoutProperty(canonicalVertex(raw, 3), "opacity")});
    writePly(path("no_rot_3.ply"), {withoutProperty(canonicalVertex(raw, 3), "rot_3")});

    auto missingOpacity = r4d::loadGaussianPly(path("no_opacity.ply"));
    ASSERT_FALSE(missingOpacity.ok());
    EXPECT_NE(missingOpacity.error().message.find("opacity"), std::string::npos)
        << missingOpacity.error().message;

    auto missingRotation = r4d::loadGaussianPly(path("no_rot_3.ply"));
    ASSERT_FALSE(missingRotation.ok());
    EXPECT_NE(missingRotation.error().message.find("rot_3"), std::string::npos)
        << missingRotation.error().message;
}

TEST_F(GaussianPly, RejectsAnShCoefficientCountThatIsNotAWholeDegree)
{
    const RawSplat raw;
    Vertex vertex = canonicalVertex(raw, 3);
    // 45 coefficients is degree 3; dropping three leaves no whole degree.
    for (int i = 42; i < 45; ++i)
    {
        vertex = withoutProperty(vertex, "f_rest_" + std::to_string(i));
    }
    writePly(path(), {vertex});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::UnsupportedFeature);
}

TEST_F(GaussianPly, RejectsAGapInTheShCoefficients)
{
    const RawSplat raw;
    writePly(path(), {withoutProperty(canonicalVertex(raw, 3), "f_rest_0")});

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::InvalidArgument);
    EXPECT_NE(loaded.error().message.find("f_rest_0"), std::string::npos)
        << loaded.error().message;
}

TEST_F(GaussianPly, RejectsAListProperty)
{
    const RawSplat raw;
    PlyOptions options;
    options.extraHeaderLines = {"property list uchar int vertex_indices"};
    writePly(path(), {canonicalVertex(raw, 3)}, options);

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::UnsupportedFeature);
}

TEST_F(GaussianPly, RejectsASecondElement)
{
    // Guessing where a second element's body begins is how offsets go wrong
    // silently, so this is refused rather than skipped.
    const RawSplat raw;
    PlyOptions options;
    options.extraHeaderLines = {"element face 1", "property uchar other"};
    writePly(path(), {canonicalVertex(raw, 3)}, options);

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::UnsupportedFeature);
    EXPECT_NE(loaded.error().message.find("face"), std::string::npos)
        << loaded.error().message;
}

TEST_F(GaussianPly, RejectsAnUnknownPropertyType)
{
    const RawSplat raw;
    Vertex vertex = canonicalVertex(raw, 3);
    writePly(path(), {vertex});

    // Rewrite one declaration in place: the writer only emits types it can pack.
    std::string contents;
    {
        std::ifstream file{path(), std::ios::binary};
        contents.assign(std::istreambuf_iterator<char>{file},
                        std::istreambuf_iterator<char>{});
    }
    const std::string from = "property float nx";
    const auto at = contents.find(from);
    ASSERT_NE(at, std::string::npos);
    contents.replace(at, from.size(), "property quad8 nx");
    {
        std::ofstream file{path(), std::ios::binary | std::ios::trunc};
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    auto loaded = r4d::loadGaussianPly(path());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, r4d::ErrorCode::UnsupportedFeature);
}
