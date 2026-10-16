// D3D12Common.hpp
#pragma once

#include <radiance4d/core/Log.hpp>
#include <radiance4d/core/Result.hpp>

// The Agility include directory is placed first, so these resolve to the
// redistributable headers rather than the Windows SDK ones.
#include <d3d12.h>
#include <dxgi1_6.h>

#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <format>
#include <string>

namespace r4d::d3d12
{

    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    std::string hresultToString(HRESULT hr);

    inline Error hresultError(ErrorCode code, HRESULT hr, std::string_view what)
    {
        return makeError(code, std::format("{}: {}", what, hresultToString(hr)),
                         static_cast<long>(hr));
    }

} // namespace r4d::d3d12

#define R4D_HR(expr, what)                                                               \
    do                                                                                   \
    {                                                                                    \
        const HRESULT _r4d_hr = (expr);                                                  \
        if (FAILED(_r4d_hr))                                                             \
        {                                                                                \
            return ::r4d::d3d12::hresultError(::r4d::ErrorCode::ApiCallFailed, _r4d_hr,  \
                                              (what));                                   \
        }                                                                                \
    } while (false)

#define R4D_HR_DEVICE(expr, what)                                                        \
    do                                                                                   \
    {                                                                                    \
        const HRESULT _r4d_hr = (expr);                                                  \
        if (FAILED(_r4d_hr))                                                             \
        {                                                                                \
            return ::r4d::d3d12::hresultError(::r4d::ErrorCode::DeviceCreationFailed,    \
                                              _r4d_hr, (what));                          \
        }                                                                                \
    } while (false)
