// D3D12Adapter.hpp
#pragma once

#include "D3D12Common.hpp"

#include <radiance4d/graphics/AdapterInfo.hpp>
#include <radiance4d/graphics/GpuCapabilities.hpp>

#include <string>

namespace r4d::d3d12
{

    struct SelectedAdapter
    {
        ComPtr<IDXGIAdapter4> adapter;
        AdapterInfo info;
    };

    // An empty selector means the highest-performance hardware adapter. Software
    // adapters are returned only when asked for by name.
    Result<SelectedAdapter> selectAdapter(IDXGIFactory6* factory,
                                          const std::string& selector);

    GpuCapabilities queryCapabilities(ID3D12Device* device, IDXGIFactory5* factory);

} // namespace r4d::d3d12
