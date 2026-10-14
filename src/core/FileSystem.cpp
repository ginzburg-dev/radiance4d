// FileSystem.cpp
#include <radiance4d/core/FileSystem.hpp>

#include <windows.h>

#include <format>
#include <fstream>

namespace r4d
{

    const std::filesystem::path& executableDirectory()
    {
        static const std::filesystem::path dir = []() -> std::filesystem::path {
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD written = ::GetModuleFileNameW(
                    nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (written == 0)
                {
                    return std::filesystem::current_path();
                }
                if (written < buffer.size())
                {
                    buffer.resize(written);
                    break;
                }
                buffer.resize(buffer.size() * 2);
            }
            return std::filesystem::path{buffer}.parent_path();
        }();
        return dir;
    }

    Result<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            return makeError(ErrorCode::NotFound,
                             std::format("cannot stat '{}': {}", path.string(), ec.message()));
        }

        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            return makeError(ErrorCode::IoFailure,
                             std::format("cannot open '{}'", path.string()));
        }

        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (size > 0 &&
            !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        {
            return makeError(ErrorCode::IoFailure,
                             std::format("short read on '{}'", path.string()));
        }
        return bytes;
    }

} // namespace r4d
