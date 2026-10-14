// D3D12Adapter.cpp
#include "D3D12Adapter.hpp"

#include <radiance4d/core/FileSystem.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <vector>

namespace r4d::d3d12
{
    namespace
    {
        std::string wideToUtf8(const wchar_t* text)
        {
            if (text == nullptr || *text == L'\0')
            {
                return {};
            }
            const int needed =
                ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (needed <= 1)
            {
                return {};
            }
            std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), needed, nullptr, nullptr);
            return utf8;
        }

        std::string toLower(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            std::transform(text.begin(), text.end(), std::back_inserter(out),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        AdapterInfo describeAdapter(IDXGIAdapter4* adapter)
        {
            DXGI_ADAPTER_DESC3 desc{};
            AdapterInfo info;
            if (SUCCEEDED(adapter->GetDesc3(&desc)))
            {
                info.name = wideToUtf8(desc.Description);
                info.vendorId = desc.VendorId;
                info.deviceId = desc.DeviceId;
                info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
                info.dedicatedSystemMemory = desc.DedicatedSystemMemory;
                info.sharedSystemMemory = desc.SharedSystemMemory;
                info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
            }
            return info;
        }

        // An adapter can enumerate and still refuse device creation.
        bool supportsD3D12(IDXGIAdapter4* adapter)
        {
            return SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                                 _uuidof(ID3D12Device), nullptr));
        }

        // Path of the loaded D3D12Core.dll, or empty.
        std::string loadedD3D12CorePath()
        {
            const HMODULE module = ::GetModuleHandleW(L"D3D12Core.dll");
            if (module == nullptr)
            {
                return {};
            }
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD written =
                    ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (written == 0)
                {
                    return {};
                }
                if (written < buffer.size())
                {
                    buffer.resize(written);
                    break;
                }
                buffer.resize(buffer.size() * 2);
            }
            return wideToUtf8(buffer.c_str());
        }

        // What the runtime reports, not what we compiled against.
        std::uint32_t queryRuntimeSdkVersion(ID3D12Device* device)
        {
            ComPtr<ID3D12DeviceConfiguration> config;
            if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&config))))
            {
                const D3D12_DEVICE_CONFIGURATION_DESC desc = config->GetDesc();
                return static_cast<std::uint32_t>(desc.SDKVersion);
            }
            // ID3D12DeviceConfiguration is itself an Agility-era interface; its
            // absence means we are on an older inbox runtime.
            return 0;
        }

        std::string describeLoadedRuntime(std::uint32_t runtimeSdkVersion)
        {
            const std::string corePath = loadedD3D12CorePath();
            std::string origin = "unknown";
            if (!corePath.empty())
            {
                const std::string exeDir = executableDirectory().string();
                const std::string loweredPath = toLower(corePath);
                if (loweredPath.find(toLower(exeDir)) == 0)
                {
                    origin = "Agility SDK redistributable (shipped alongside the executable)";
                }
                else
                {
                    origin = "operating system runtime";
                }
            }

            std::string built;
#if R4D_ENABLE_AGILITY_SDK
            built = std::format(", built against D3D12_SDK_VERSION={}", R4D_AGILITY_SDK_VERSION);
#endif
            return std::format("{} [{}], reported SDKVersion={}{}",
                               origin,
                               corePath.empty() ? "D3D12Core.dll not loaded" : corePath,
                               runtimeSdkVersion,
                               built);
        }

        std::string featureLevelToString(D3D_FEATURE_LEVEL level)
        {
            switch (level)
            {
                case D3D_FEATURE_LEVEL_11_0: return "11_0";
                case D3D_FEATURE_LEVEL_11_1: return "11_1";
                case D3D_FEATURE_LEVEL_12_0: return "12_0";
                case D3D_FEATURE_LEVEL_12_1: return "12_1";
                case D3D_FEATURE_LEVEL_12_2: return "12_2";
                default:
                    return std::format("0x{:04X}", static_cast<unsigned>(level));
            }
        }
    } // namespace

    std::string hresultToString(HRESULT hr)
    {
        char* buffer = nullptr;
        const DWORD written = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(hr),
            MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
            reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

        std::string message;
        if (written != 0 && buffer != nullptr)
        {
            message.assign(buffer, written);
            while (!message.empty() &&
                   (message.back() == '\n' || message.back() == '\r' || message.back() == '.'))
            {
                message.pop_back();
            }
        }
        if (buffer != nullptr)
        {
            ::LocalFree(buffer);
        }

        if (message.empty())
        {
            return std::format("HRESULT 0x{:08X}", static_cast<unsigned>(hr));
        }
        return std::format("HRESULT 0x{:08X} ({})", static_cast<unsigned>(hr), message);
    }

    Result<SelectedAdapter> selectAdapter(IDXGIFactory6* factory, const std::string& selector)
    {
        const std::string wanted = toLower(selector);

        if (wanted == "warp")
        {
            ComPtr<IDXGIAdapter> warp;
            R4D_HR_DEVICE(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),
                          "IDXGIFactory6::EnumWarpAdapter");
            ComPtr<IDXGIAdapter4> warp4;
            R4D_HR_DEVICE(warp.As(&warp4), "IDXGIAdapter -> IDXGIAdapter4");
            SelectedAdapter selected;
            selected.adapter = warp4;
            selected.info = describeAdapter(warp4.Get());
            selected.info.isSoftware = true;
            R4D_INFO("Adapter: using WARP software adapter by request");
            return selected;
        }

        // Most performant first; log every candidate so a wrong pick is visible.
        struct Candidate
        {
            ComPtr<IDXGIAdapter4> adapter;
            AdapterInfo info;
            bool usable = false;
        };
        std::vector<Candidate> candidates;

        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter4> adapter;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(hr))
            {
                return hresultError(ErrorCode::DeviceCreationFailed, hr,
                                    "IDXGIFactory6::EnumAdapterByGpuPreference");
            }

            Candidate candidate;
            candidate.adapter = adapter;
            candidate.info = describeAdapter(adapter.Get());
            candidate.usable = supportsD3D12(adapter.Get());
            R4D_INFO("Adapter[{}]: {}{}", index, candidate.info.ToString(),
                     candidate.usable ? "" : " -- cannot create a D3D12 device, skipping");
            candidates.push_back(std::move(candidate));
        }

        if (candidates.empty())
        {
            return makeError(ErrorCode::DeviceCreationFailed,
                             "no DXGI adapters were enumerated");
        }

        if (!wanted.empty() &&
            std::all_of(wanted.begin(), wanted.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; }))
        {
            std::size_t index = 0;
            const auto parsed = std::from_chars(
                wanted.data(), wanted.data() + wanted.size(), index);
            if (parsed.ec != std::errc{} || index >= candidates.size())
            {
                return makeError(ErrorCode::InvalidArgument,
                                 std::format("adapter index '{}' is out of range (0..{})",
                                             selector, candidates.size() - 1));
            }
            if (!candidates[index].usable)
            {
                return makeError(ErrorCode::DeviceCreationFailed,
                                 std::format("adapter {} ({}) cannot create a D3D12 device",
                                             index, candidates[index].info.name));
            }
            SelectedAdapter selected;
            selected.adapter = candidates[index].adapter;
            selected.info = candidates[index].info;
            return selected;
        }

        if (!wanted.empty())
        {
            for (auto& candidate : candidates)
            {
                if (candidate.usable &&
                    toLower(candidate.info.name).find(wanted) != std::string::npos)
                {
                    SelectedAdapter selected;
                    selected.adapter = candidate.adapter;
                    selected.info = candidate.info;
                    R4D_INFO("Adapter: matched '{}' by name", selector);
                    return selected;
                }
            }
            return makeError(ErrorCode::NotFound,
                             std::format("no usable adapter matched '{}'", selector));
        }

        // Never fall back to a software adapter implicitly.
        for (auto& candidate : candidates)
        {
            if (candidate.usable && !candidate.info.isSoftware)
            {
                SelectedAdapter selected;
                selected.adapter = candidate.adapter;
                selected.info = candidate.info;
                return selected;
            }
        }

        return makeError(ErrorCode::DeviceCreationFailed,
                         "no usable hardware adapter found; pass --adapter warp to use "
                         "the software adapter explicitly");
    }

    GpuCapabilities queryCapabilities(ID3D12Device* device, IDXGIFactory5* factory)
    {
        GpuCapabilities caps;

        static constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
        levels.NumFeatureLevels = static_cast<UINT>(std::size(kLevels));
        levels.pFeatureLevelsRequested = kLevels;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                                  sizeof(levels))))
        {
            caps.featureLevel = featureLevelToString(levels.MaxSupportedFeatureLevel);
        }

        // Walk down numerically; the enumerators vary between SDKs.
        for (unsigned sm = 0x69; sm >= 0x60; --sm)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
            shaderModel.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(sm);
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                                      &shaderModel, sizeof(shaderModel))))
            {
                const unsigned actual = static_cast<unsigned>(shaderModel.HighestShaderModel);
                caps.highestShaderModel =
                    std::format("{}_{}", (actual >> 4) & 0xF, actual & 0xF);
                break;
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                  sizeof(options))))
        {
            caps.resourceBindingTier = static_cast<std::uint32_t>(options.ResourceBindingTier);
            caps.resourceHeapTier = static_cast<std::uint32_t>(options.ResourceHeapTier);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                                  sizeof(options12))))
        {
            caps.enhancedBarriersSupported = options12.EnhancedBarriersSupported != FALSE;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                                  sizeof(options5))))
        {
            caps.rayTracingSupported =
                options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                                  sizeof(options7))))
        {
            caps.meshShadersSupported =
                options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        }

        if (factory != nullptr)
        {
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                       &allowTearing, sizeof(allowTearing))))
            {
                caps.tearingSupported = allowTearing != FALSE;
            }
        }

        // Windows ships its own d3d12core.dll, so only the loaded module path
        // tells us whether the redistributable won.
        caps.agilitySdkVersion = queryRuntimeSdkVersion(device);
        caps.runtimeDescription = describeLoadedRuntime(caps.agilitySdkVersion);

        return caps;
    }

} // namespace r4d::d3d12
