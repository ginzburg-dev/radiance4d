// FileSystem.hpp
#pragma once

#include <radiance4d/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace r4d
{

    // Shaders and the D3D12 redistributable are located relative to this, not to
    // the current working directory.
    const std::filesystem::path& executableDirectory();

    Result<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path);

} // namespace r4d
