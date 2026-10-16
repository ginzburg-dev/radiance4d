// Result.cpp
#include <radiance4d/core/Result.hpp>

#include <format>

namespace r4d
{

    std::string Error::ToString() const
    {
        if (hresult != 0)
        {
            return std::format("{} (hr=0x{:08X}): {}", r4d::ToString(code),
                               static_cast<unsigned long>(hresult), message);
        }
        return std::format("{}: {}", r4d::ToString(code), message);
    }

} // namespace r4d
