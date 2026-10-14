// D3D12Renderer.cpp
#include "D3D12Renderer.hpp"

#include <radiance4d/core/FileSystem.hpp>

#include <cstring>

namespace r4d::d3d12
{
    namespace
    {
        struct Vertex
        {
            float position[3];
            float colour[3];
        };

        // Triangle then quad, in one buffer.
        constexpr Vertex kVertices[] = {
            // Triangle
            {{ 0.0f,  0.6f, 0.0f}, {1.0f, 0.15f, 0.15f}},
            {{ 0.6f, -0.5f, 0.0f}, {0.15f, 1.0f, 0.25f}},
            {{-0.6f, -0.5f, 0.0f}, {0.2f, 0.35f, 1.0f}},
            // Quad
            {{-0.85f,  0.85f, 0.0f}, {0.16f, 0.18f, 0.28f}},
            {{ 0.85f,  0.85f, 0.0f}, {0.10f, 0.26f, 0.38f}},
            {{ 0.85f, -0.85f, 0.0f}, {0.22f, 0.12f, 0.30f}},
            {{-0.85f, -0.85f, 0.0f}, {0.08f, 0.10f, 0.16f}},
        };

        constexpr std::uint16_t kIndices[] = {0, 1, 2, 0, 2, 3};

        constexpr UINT kTriangleVertexCount = 3;
        constexpr UINT kQuadVertexOffset = 3;

        struct FrameConstants
        {
            Mat4 transform;
            float timeSeconds;
            float padding[3];
        };

        constexpr UINT64 kConstantBufferAlignment = 256;

        // Must match the optimised clear value the render target is created with.
        constexpr float kClearColour[4] = {0.043f, 0.047f, 0.063f, 1.0f};

        constexpr UINT64 alignUp(UINT64 value, UINT64 alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES props{};
            props.Type = type;
            props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            props.CreationNodeMask = 1;
            props.VisibleNodeMask = 1;
            return props;
        }

        D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Alignment = 0;
            desc.Width = bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            return desc;
        }

        D3D12_TEXTURE_BARRIER textureBarrier(ID3D12Resource* resource,
                                             D3D12_BARRIER_SYNC syncBefore,
                                             D3D12_BARRIER_SYNC syncAfter,
                                             D3D12_BARRIER_ACCESS accessBefore,
                                             D3D12_BARRIER_ACCESS accessAfter,
                                             D3D12_BARRIER_LAYOUT layoutBefore,
                                             D3D12_BARRIER_LAYOUT layoutAfter)
        {
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = syncBefore;
            barrier.SyncAfter = syncAfter;
            barrier.AccessBefore = accessBefore;
            barrier.AccessAfter = accessAfter;
            barrier.LayoutBefore = layoutBefore;
            barrier.LayoutAfter = layoutAfter;
            barrier.pResource = resource;
            barrier.Subresources = D3D12_BARRIER_SUBRESOURCE_RANGE{0, 1, 0, 1, 0, 1};
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            return barrier;
        }

        void submitTextureBarrier(ID3D12GraphicsCommandList7* commandList,
                                  const D3D12_TEXTURE_BARRIER& barrier)
        {
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            commandList->Barrier(1, &group);
        }

        D3D12_RASTERIZER_DESC defaultRasterizerDesc()
        {
            D3D12_RASTERIZER_DESC desc{};
            desc.FillMode = D3D12_FILL_MODE_SOLID;
            // Culling comes with real meshes in M2.
            desc.CullMode = D3D12_CULL_MODE_NONE;
            desc.FrontCounterClockwise = FALSE;
            desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
            desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            desc.DepthClipEnable = TRUE;
            desc.MultisampleEnable = FALSE;
            desc.AntialiasedLineEnable = FALSE;
            desc.ForcedSampleCount = 0;
            desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            return desc;
        }

        D3D12_BLEND_DESC opaqueBlendDesc()
        {
            D3D12_BLEND_DESC desc{};
            desc.AlphaToCoverageEnable = FALSE;
            desc.IndependentBlendEnable = FALSE;
            for (auto& rt : desc.RenderTarget)
            {
                rt.BlendEnable = FALSE;
                rt.LogicOpEnable = FALSE;
                rt.SrcBlend = D3D12_BLEND_ONE;
                rt.DestBlend = D3D12_BLEND_ZERO;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            return desc;
        }
    } // namespace

    Result<std::unique_ptr<Renderer>> createRenderer(const RendererDesc& desc)
    {
        return D3D12Renderer::create(desc);
    }

    Result<std::unique_ptr<Renderer>> D3D12Renderer::create(const RendererDesc& desc)
    {
        auto renderer = std::unique_ptr<D3D12Renderer>{new D3D12Renderer{}};
        if (auto status = renderer->initialise(desc); !status)
        {
            return status.error();
        }
        return std::unique_ptr<Renderer>{renderer.release()};
    }

    D3D12Renderer::~D3D12Renderer()
    {
        // Drain before releasing anything the GPU may still read; the validation
        // sink has to outlive the device to catch teardown messages.
        if (m_device && m_fence && m_fenceEvent != nullptr)
        {
            waitForGpuIdle();
        }

        for (auto& frame : m_frames)
        {
            if (frame.constantBuffer && frame.constantBufferCpu != nullptr)
            {
                frame.constantBuffer->Unmap(0, nullptr);
                frame.constantBufferCpu = nullptr;
            }
        }

        if (m_fenceEvent != nullptr)
        {
            ::CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
        // m_frameLatencyWaitable is owned by the swapchain; it must not be closed.
        m_frameLatencyWaitable = nullptr;

        releaseBackBuffers();

        m_validation.detach();

        // Before reportLiveObjects, or these always look leaked.
        m_profiler.shutdown();

        m_captureReadback.Reset();
        m_offscreenTarget.Reset();
        m_vertexBuffer.Reset();
        m_indexBuffer.Reset();
        m_pipelineState.Reset();
        m_rootSignature.Reset();
        m_commandList.Reset();
        m_frames = {};
        m_fence.Reset();
        m_rtvHeap.Reset();
        m_swapChain.Reset();
        m_queue.Reset();
        m_device.Reset();
        m_factory.Reset();

        if (m_debugLayerActive)
        {
            reportLiveObjects();
        }
    }

    Status D3D12Renderer::initialise(const RendererDesc& desc)
    {
        m_headless = (desc.windowHandle == nullptr);
        m_vsync = desc.vsync;
        m_fixedTimeStep = desc.fixedTimeStep;
        m_size = desc.initialSize;

        if (m_size.x == 0 || m_size.y == 0)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "RendererDesc::initialSize must be non-zero");
        }

        R4D_TRY(createDeviceAndQueue(desc));
        R4D_TRY(createRtvHeap());

        if (m_headless)
        {
            R4D_INFO("Renderer: headless -- no swapchain, rendering to an offscreen target");
            R4D_TRY(createOffscreenTarget(m_size));
        }
        else
        {
            R4D_TRY(createSwapChain(desc.windowHandle, desc.initialSize));
        }

        R4D_TRY(createFrameResources());

        std::filesystem::path shaderDirectory;
        if (!desc.shaderDirectory.empty())
        {
            shaderDirectory = desc.shaderDirectory;
        }
        else
        {
            const auto beside = executableDirectory() / "shaders";
            std::error_code ec;
            if (std::filesystem::is_directory(beside, ec))
            {
                shaderDirectory = beside;
            }
#ifdef R4D_SHADER_DIR
            else
            {
                shaderDirectory = R4D_SHADER_DIR;
            }
#endif
        }
        R4D_INFO("Renderer: shader directory '{}'", shaderDirectory.string());

        R4D_TRY(createPipeline(shaderDirectory));
        R4D_TRY(createGeometry());

        // Missing timestamp support is not fatal; stats report them as invalid.
        R4D_TRY(m_profiler.initialise(m_device.Get(), m_queue.Get(), kFramesInFlight));

        if (m_fixedTimeStep > 0.0)
        {
            R4D_INFO("Renderer: deterministic animation, {:.6f} s per frame",
                     m_fixedTimeStep);
        }
        R4D_INFO("Renderer: initialised, {} {}, {} frames in flight",
                 m_headless ? 1u : kBackBufferCount,
                 m_headless ? "offscreen target" : "back buffers", kFramesInFlight);
        return Status::Ok();
    }

    Status D3D12Renderer::createRtvHeap()
    {
        // Headless uses slot 0 only.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = kBackBufferCount;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        R4D_HR(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)),
               "ID3D12Device::CreateDescriptorHeap (RTV)");
        m_rtvHeap->SetName(L"Radiance4D RTV Heap");
        m_rtvDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return Status::Ok();
    }

    Status D3D12Renderer::createOffscreenTarget(UVec2 size)
    {
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size.x;
        desc.Height = size.y;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kBackBufferFormat;   // same format as the swapchain path
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);

        // CreateCommittedResource3 takes an explicit barrier layout; a texture
        // created in a legacy state only interops with enhanced barriers from
        // COMMON. It stays in RENDER_TARGET between frames, so the frame loop
        // needs no barrier here -- captureFrame does its own round trip.
        D3D12_CLEAR_VALUE optimisedClear{};
        optimisedClear.Format = kBackBufferFormat;
        optimisedClear.Color[0] = kClearColour[0];
        optimisedClear.Color[1] = kClearColour[1];
        optimisedClear.Color[2] = kClearColour[2];
        optimisedClear.Color[3] = kClearColour[3];

        R4D_HR(m_device->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                                                  &optimisedClear, nullptr, 0, nullptr,
                                                  IID_PPV_ARGS(&m_offscreenTarget)),
               "CreateCommittedResource3 (offscreen render target)");
        m_offscreenTarget->SetName(L"Radiance4D Offscreen Target");

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = kBackBufferFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_offscreenTarget.Get(), &rtvDesc,
                                         m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        m_size = size;
        R4D_INFO("Offscreen target: {}x{}", size.x, size.y);
        return Status::Ok();
    }

    ID3D12Resource* D3D12Renderer::currentRenderTarget(UINT backBufferIndex) const
    {
        return m_headless ? m_offscreenTarget.Get() : m_backBuffers[backBufferIndex].Get();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::currentRenderTargetView(
        UINT backBufferIndex) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        if (!m_headless)
        {
            rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * m_rtvDescriptorSize;
        }
        return rtv;
    }

    Status D3D12Renderer::createDeviceAndQueue(const RendererDesc& desc)
    {
        // Both are ignored unless set before device creation.
        if (desc.enableDebugLayer)
        {
            m_debugLayerActive = enableDebugLayer(desc.enableGpuValidation);
            enableDeviceRemovedExtendedData();
        }

        UINT factoryFlags = 0;
        if (m_debugLayerActive)
        {
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
        R4D_HR_DEVICE(::CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)),
                      "CreateDXGIFactory2");

        auto selected = selectAdapter(m_factory.Get(), desc.adapterSelector);
        if (!selected)
        {
            return selected.error();
        }
        m_adapterInfo = selected.value().info;
        R4D_INFO("Adapter: SELECTED {}", m_adapterInfo.ToString());

        // 12_0 is the floor; fall back only with a warning.
        HRESULT hr = ::D3D12CreateDevice(selected.value().adapter.Get(),
                                         D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
        if (FAILED(hr))
        {
            R4D_WARN("Device: feature level 12_0 refused ({}); retrying at 11_0",
                     hresultToString(hr));
            hr = ::D3D12CreateDevice(selected.value().adapter.Get(),
                                     D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        }
        if (FAILED(hr))
        {
            return hresultError(ErrorCode::DeviceCreationFailed, hr, "D3D12CreateDevice");
        }
        m_device->SetName(L"Radiance4D Device");

        // Attach early so setup messages are not missed.
        m_validation.attach(m_device.Get());

        m_capabilities = queryCapabilities(m_device.Get(), m_factory.Get());
        R4D_INFO("Device: {}", m_capabilities.ToString());
        R4D_INFO("Device: runtime = {}", m_capabilities.runtimeDescription);

        // Required, not optional: dropping to legacy barriers would be a design
        // decision, not something to do silently.
        if (!m_capabilities.enhancedBarriersSupported)
        {
            return makeError(
                ErrorCode::UnsupportedFeature,
                std::format("this device/runtime does not report Enhanced Barriers "
                            "support (D3D12_FEATURE_D3D12_OPTIONS12."
                            "EnhancedBarriersSupported is FALSE). Adapter: {}. Runtime: {}. "
                            "Radiance4D requires Enhanced Barriers; falling back to legacy "
                            "barriers is a design decision that has not been taken.",
                            m_adapterInfo.name, m_capabilities.runtimeDescription));
        }
        m_allowTearing = m_capabilities.tearingSupported;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;
        R4D_HR_DEVICE(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)),
                      "ID3D12Device::CreateCommandQueue");
        m_queue->SetName(L"Radiance4D Direct Queue");

        return Status::Ok();
    }

    Status D3D12Renderer::createSwapChain(void* windowHandle, UVec2 size)
    {
        const HWND hwnd = static_cast<HWND>(windowHandle);

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = (size.x > 0) ? size.x : 1;
        desc.Height = (size.y > 0) ? size.y : 1;
        desc.Format = kBackBufferFormat;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;   // the flip model forbids multisampled swapchains
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBackBufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (m_allowTearing)
        {
            desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        ComPtr<IDXGISwapChain1> swapChain1;
        R4D_HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &desc, nullptr,
                                                 nullptr, &swapChain1),
               "IDXGIFactory6::CreateSwapChainForHwnd");
        R4D_HR(swapChain1.As(&m_swapChain), "IDXGISwapChain1 -> IDXGISwapChain3");

        // Keep DXGI off Alt+Enter.
        R4D_HR(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
               "IDXGIFactory::MakeWindowAssociation");

        R4D_HR(m_swapChain->SetMaximumFrameLatency(kFramesInFlight),
               "IDXGISwapChain3::SetMaximumFrameLatency");
        m_frameLatencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();
        if (m_frameLatencyWaitable == nullptr)
        {
            return makeError(ErrorCode::ApiCallFailed,
                             "GetFrameLatencyWaitableObject returned null");
        }

        R4D_TRY(acquireBackBuffers());

        DXGI_SWAP_CHAIN_DESC1 actual{};
        if (SUCCEEDED(m_swapChain->GetDesc1(&actual)))
        {
            m_size = UVec2{actual.Width, actual.Height};
        }
        R4D_INFO("Swapchain: {}x{}, {} buffers, flip-discard, tearing {}",
                 m_size.x, m_size.y, kBackBufferCount, m_allowTearing ? "allowed" : "off");
        return Status::Ok();
    }

    Status D3D12Renderer::acquireBackBuffers()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBufferCount; ++i)
        {
            R4D_HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])),
                   "IDXGISwapChain::GetBuffer");
            const std::wstring name = L"Radiance4D BackBuffer " + std::to_wstring(i);
            m_backBuffers[i]->SetName(name.c_str());

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = kBackBufferFormat;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), &rtvDesc, handle);
            handle.ptr += m_rtvDescriptorSize;
        }
        return Status::Ok();
    }

    void D3D12Renderer::releaseBackBuffers()
    {
        for (auto& buffer : m_backBuffers)
        {
            buffer.Reset();
        }
    }

    Status D3D12Renderer::createFrameResources()
    {
        const UINT64 constantBufferSize =
            alignUp(sizeof(FrameConstants), kConstantBufferAlignment);

        for (UINT i = 0; i < kFramesInFlight; ++i)
        {
            auto& frame = m_frames[i];

            R4D_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&frame.commandAllocator)),
                   "ID3D12Device::CreateCommandAllocator");
            const std::wstring allocatorName =
                L"Radiance4D Command Allocator (frame slot " + std::to_wstring(i) + L")";
            frame.commandAllocator->SetName(allocatorName.c_str());

            // Per frame in flight, not per back buffer.
            const auto uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = bufferDesc(constantBufferSize);
            R4D_HR(m_device->CreateCommittedResource(
                       &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                       IID_PPV_ARGS(&frame.constantBuffer)),
                   "CreateCommittedResource (frame constant buffer)");
            const std::wstring cbName =
                L"Radiance4D Frame Constants (slot " + std::to_wstring(i) + L")";
            frame.constantBuffer->SetName(cbName.c_str());

            void* mapped = nullptr;
            const D3D12_RANGE noRead{0, 0};   // write-only from the CPU
            R4D_HR(frame.constantBuffer->Map(0, &noRead, &mapped),
                   "ID3D12Resource::Map (frame constant buffer)");
            frame.constantBufferCpu = static_cast<std::byte*>(mapped);

            frame.fenceValue = 0;
        }

        R4D_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
               "ID3D12Device::CreateFence");
        m_fence->SetName(L"Radiance4D Frame Fence");

        m_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr)
        {
            return makeError(ErrorCode::ApiCallFailed, "CreateEventW for the frame fence",
                             static_cast<long>(::GetLastError()));
        }

        // Created closed, so the first use needs no special case.
        R4D_HR(m_device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            D3D12_COMMAND_LIST_FLAG_NONE,
                                            IID_PPV_ARGS(&m_commandList)),
               "ID3D12Device::CreateCommandList1 (requires ID3D12GraphicsCommandList7 "
               "for Enhanced Barriers)");
        m_commandList->SetName(L"Radiance4D Command List");

        return Status::Ok();
    }

    Result<std::vector<std::byte>> D3D12Renderer::loadShaderBlob(
        const std::filesystem::path& shaderDirectory, std::string_view name) const
    {
        const auto path = shaderDirectory / name;
        auto blob = readBinaryFile(path);
        if (!blob)
        {
            return makeError(ErrorCode::ShaderLoadFailed,
                             std::format("cannot read compiled shader '{}': {}. Shaders "
                                         "are compiled at build time; check that the "
                                         "r4d_shaders target ran.",
                                         path.string(), blob.error().message));
        }
        if (blob.value().empty())
        {
            return makeError(ErrorCode::ShaderLoadFailed,
                             std::format("compiled shader '{}' is empty", path.string()));
        }
        return blob.value();
    }

    Status D3D12Renderer::createPipeline(const std::filesystem::path& shaderDirectory)
    {
        auto vertexShader = loadShaderBlob(shaderDirectory, "Triangle.vs.dxil");
        if (!vertexShader)
        {
            return vertexShader.error();
        }
        auto pixelShader = loadShaderBlob(shaderDirectory, "Triangle.ps.dxil");
        if (!pixelShader)
        {
            return pixelShader.error();
        }

        // One root CBV at b0; materials in M2 will need more.
        D3D12_ROOT_PARAMETER1 rootParameter{};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameter.Descriptor.ShaderRegister = 0;
        rootParameter.Descriptor.RegisterSpace = 0;
        rootParameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = 1;
        rootDesc.Desc_1_1.pParameters = &rootParameter;
        rootDesc.Desc_1_1.NumStaticSamplers = 0;
        rootDesc.Desc_1_1.pStaticSamplers = nullptr;
        rootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialised;
        ComPtr<ID3DBlob> rootError;
        const HRESULT hr = ::D3D12SerializeVersionedRootSignature(&rootDesc, &serialised,
                                                                  &rootError);
        if (FAILED(hr))
        {
            const char* detail = rootError
                                     ? static_cast<const char*>(rootError->GetBufferPointer())
                                     : "(no detail)";
            return hresultError(ErrorCode::ApiCallFailed, hr,
                                std::format("D3D12SerializeVersionedRootSignature: {}",
                                            detail));
        }

        R4D_HR(m_device->CreateRootSignature(0, serialised->GetBufferPointer(),
                                             serialised->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rootSignature)),
               "ID3D12Device::CreateRootSignature");
        m_rootSignature->SetName(L"Radiance4D Root Signature");

        const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, colour),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS.pShaderBytecode = vertexShader.value().data();
        psoDesc.VS.BytecodeLength = vertexShader.value().size();
        psoDesc.PS.pShaderBytecode = pixelShader.value().data();
        psoDesc.PS.BytecodeLength = pixelShader.value().size();
        psoDesc.BlendState = opaqueBlendDesc();
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = defaultRasterizerDesc();
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.InputLayout.pInputElementDescs = inputLayout;
        psoDesc.InputLayout.NumElements = static_cast<UINT>(std::size(inputLayout));
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = kBackBufferFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;   // no depth buffer until M2
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        R4D_HR(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)),
               "ID3D12Device::CreateGraphicsPipelineState");
        m_pipelineState->SetName(L"Radiance4D Triangle PSO");

        return Status::Ok();
    }

    Status D3D12Renderer::createGeometry()
    {
        constexpr UINT64 vertexBytes = sizeof(kVertices);
        constexpr UINT64 indexBytes = sizeof(kIndices);

        const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        const auto uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);

        // Static geometry goes to device-local memory, uploaded once.
        const auto vbDesc = bufferDesc(vertexBytes);
        R4D_HR(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&m_vertexBuffer)),
               "CreateCommittedResource (vertex buffer)");
        m_vertexBuffer->SetName(L"Radiance4D Vertex Buffer");

        const auto ibDesc = bufferDesc(indexBytes);
        R4D_HR(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&m_indexBuffer)),
               "CreateCommittedResource (index buffer)");
        m_indexBuffer->SetName(L"Radiance4D Index Buffer");

        ComPtr<ID3D12Resource> vertexUpload;
        ComPtr<ID3D12Resource> indexUpload;
        R4D_HR(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&vertexUpload)),
               "CreateCommittedResource (vertex upload)");
        R4D_HR(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&indexUpload)),
               "CreateCommittedResource (index upload)");

        {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{0, 0};
            R4D_HR(vertexUpload->Map(0, &noRead, &mapped), "Map (vertex upload)");
            std::memcpy(mapped, kVertices, static_cast<std::size_t>(vertexBytes));
            vertexUpload->Unmap(0, nullptr);

            R4D_HR(indexUpload->Map(0, &noRead, &mapped), "Map (index upload)");
            std::memcpy(mapped, kIndices, static_cast<std::size_t>(indexBytes));
            indexUpload->Unmap(0, nullptr);
        }

        // One-shot upload on slot 0, waited on below.
        auto& frame = m_frames[0];
        R4D_HR(frame.commandAllocator->Reset(), "Reset (upload allocator)");
        R4D_HR(m_commandList->Reset(frame.commandAllocator.Get(), nullptr),
               "Reset (upload command list)");

        m_commandList->CopyBufferRegion(m_vertexBuffer.Get(), 0, vertexUpload.Get(), 0,
                                        vertexBytes);
        m_commandList->CopyBufferRegion(m_indexBuffer.Get(), 0, indexUpload.Get(), 0,
                                        indexBytes);

        // Buffers have no layout, only sync and access.
        D3D12_BUFFER_BARRIER bufferBarriers[2]{};
        bufferBarriers[0].SyncBefore = D3D12_BARRIER_SYNC_COPY;
        bufferBarriers[0].SyncAfter = D3D12_BARRIER_SYNC_VERTEX_SHADING;
        bufferBarriers[0].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
        bufferBarriers[0].AccessAfter = D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
        bufferBarriers[0].pResource = m_vertexBuffer.Get();
        bufferBarriers[0].Offset = 0;
        bufferBarriers[0].Size = UINT64_MAX;

        bufferBarriers[1] = bufferBarriers[0];
        // INDEX_BUFFER access pairs with INDEX_INPUT sync; the debug layer
        // rejects it against VERTEX_SHADING.
        bufferBarriers[1].SyncAfter = D3D12_BARRIER_SYNC_INDEX_INPUT;
        bufferBarriers[1].AccessAfter = D3D12_BARRIER_ACCESS_INDEX_BUFFER;
        bufferBarriers[1].pResource = m_indexBuffer.Get();

        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_BUFFER;
        group.NumBarriers = static_cast<UINT32>(std::size(bufferBarriers));
        group.pBufferBarriers = bufferBarriers;
        m_commandList->Barrier(1, &group);

        R4D_HR(m_commandList->Close(), "Close (upload command list)");
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_queue->ExecuteCommandLists(1, lists);

        // The upload heaps are local, so the copy must finish first.
        const UINT64 uploadFence = m_nextFenceValue++;
        R4D_HR(m_queue->Signal(m_fence.Get(), uploadFence), "Signal (upload fence)");
        R4D_TRY(waitForFenceValue(uploadFence));

        // Slot 0 has been used; record it so the allocator is not reset early.
        frame.fenceValue = uploadFence;

        m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
        m_vertexBufferView.StrideInBytes = sizeof(Vertex);

        m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
        m_indexBufferView.Format = DXGI_FORMAT_R16_UINT;

        m_triangleVertexCount = kTriangleVertexCount;
        m_quadVertexOffset = kQuadVertexOffset;
        m_quadIndexCount = static_cast<UINT>(std::size(kIndices));
        m_quadIndexOffset = 0;

        R4D_INFO("Geometry: uploaded {} vertices and {} indices to device-local memory",
                 std::size(kVertices), std::size(kIndices));
        return Status::Ok();
    }

    Status D3D12Renderer::waitForFenceValue(UINT64 value)
    {
        if (m_fence->GetCompletedValue() >= value)
        {
            return Status::Ok();
        }
        R4D_HR(m_fence->SetEventOnCompletion(value, m_fenceEvent),
               "ID3D12Fence::SetEventOnCompletion");
        const DWORD waited = ::WaitForSingleObjectEx(m_fenceEvent, 10000, FALSE);
        if (waited != WAIT_OBJECT_0)
        {
            // A timeout usually means a hang; grab DRED while it is available.
            const HRESULT removal = m_device->GetDeviceRemovedReason();
            if (FAILED(removal))
            {
                logDeviceRemovedDiagnostics(m_device.Get(), removal);
            }
            return makeError(ErrorCode::ApiCallFailed,
                             std::format("timed out waiting for fence value {} (completed "
                                         "{})", value, m_fence->GetCompletedValue()));
        }
        return Status::Ok();
    }

    void D3D12Renderer::waitForGpuIdle()
    {
        if (!m_queue || !m_fence)
        {
            return;
        }
        const UINT64 value = m_nextFenceValue++;
        if (FAILED(m_queue->Signal(m_fence.Get(), value)))
        {
            return;
        }
        if (auto status = waitForFenceValue(value); !status)
        {
            R4D_ERROR("waitForGpuIdle: {}", status.error().ToString());
        }
    }

    Status D3D12Renderer::renderFrame()
    {
        if (m_size.x == 0 || m_size.y == 0)
        {
            // Minimised.
            return Status::Ok();
        }

        // Bound input latency before doing any CPU work for this frame.
        if (m_frameLatencyWaitable != nullptr)
        {
            ::WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);
        }

        const UINT frameIndex = static_cast<UINT>(m_frameCounter % kFramesInFlight);
        auto& frame = m_frames[frameIndex];

        // Allocator reuse is gated on the fence, not the back-buffer index.
        R4D_TRY(waitForFenceValue(frame.fenceValue));

        // The wait above also means this slot's timestamps are resolved.
        double gpuMilliseconds = 0.0;
        if (m_profiler.readFrameTimeMs(frameIndex, gpuMilliseconds))
        {
            m_gpuTimeHistory.add(gpuMilliseconds);
            m_stats.gpuFrameMs = gpuMilliseconds;
            m_stats.gpuFrameAverageMs = m_gpuTimeHistory.average();
            m_stats.gpuTimingsValid = true;
        }

        const UINT backBufferIndex =
            m_headless ? 0u : m_swapChain->GetCurrentBackBufferIndex();

        const double cpuDelta = m_frameTimer.tick();
        // Fixed step makes a capture depend on the frame number alone.
        const float timeSeconds =
            (m_fixedTimeStep > 0.0)
                ? static_cast<float>(static_cast<double>(m_frameCounter) * m_fixedTimeStep)
                : static_cast<float>(m_frameTimer.totalSeconds());

        // Safe to write: the fence wait proved the GPU is done with this slot.
        {
            const float aspect = static_cast<float>(m_size.x) / static_cast<float>(m_size.y);
            const Mat4 projection =
                glm::perspective(degreesToRadians(60.0f), aspect, 0.1f, 100.0f);
            const Mat4 view =
                glm::lookAt(Vec3{0.0f, 0.0f, -2.5f}, Vec3{0.0f, 0.0f, 0.0f},
                            Vec3{0.0f, 1.0f, 0.0f});
            const Mat4 model =
                glm::rotate(Mat4{1.0f}, timeSeconds * 0.8f, Vec3{0.0f, 1.0f, 0.0f});

            FrameConstants constants{};
            constants.transform = projection * view * model;
            constants.timeSeconds = timeSeconds;
            std::memcpy(frame.constantBufferCpu, &constants, sizeof(constants));
        }

        R4D_HR(frame.commandAllocator->Reset(), "ID3D12CommandAllocator::Reset");
        R4D_HR(m_commandList->Reset(frame.commandAllocator.Get(), m_pipelineState.Get()),
               "ID3D12GraphicsCommandList::Reset");

        m_profiler.beginFrame(m_commandList.Get(), frameIndex);

        ID3D12Resource* renderTarget = currentRenderTarget(backBufferIndex);

        // The offscreen target stays in RENDER_TARGET, so it needs no transition.
        if (!m_headless)
        {
            submitTextureBarrier(
                m_commandList.Get(),
                textureBarrier(renderTarget,
                               D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                               D3D12_BARRIER_ACCESS_NO_ACCESS,
                               D3D12_BARRIER_ACCESS_RENDER_TARGET,
                               D3D12_BARRIER_LAYOUT_PRESENT,
                               D3D12_BARRIER_LAYOUT_RENDER_TARGET));
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = currentRenderTargetView(backBufferIndex);

        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_commandList->ClearRenderTargetView(rtv, kClearColour, 0, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(m_size.x);
        viewport.Height = static_cast<float>(m_size.y);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_commandList->RSSetViewports(1, &viewport);

        D3D12_RECT scissor{0, 0, static_cast<LONG>(m_size.x), static_cast<LONG>(m_size.y)};
        m_commandList->RSSetScissorRects(1, &scissor);

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetGraphicsRootConstantBufferView(
            0, frame.constantBuffer->GetGPUVirtualAddress());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);

        std::uint32_t drawCalls = 0;
        std::uint32_t triangles = 0;

        // No depth buffer yet, so draw order decides: quad behind, triangle on top.
        m_commandList->IASetIndexBuffer(&m_indexBufferView);
        m_commandList->DrawIndexedInstanced(m_quadIndexCount, 1, m_quadIndexOffset,
                                            static_cast<INT>(m_quadVertexOffset), 0);
        ++drawCalls;
        triangles += m_quadIndexCount / 3;

        m_commandList->DrawInstanced(m_triangleVertexCount, 1, 0, 0);
        ++drawCalls;
        triangles += m_triangleVertexCount / 3;

        if (!m_headless)
        {
            submitTextureBarrier(
                m_commandList.Get(),
                textureBarrier(renderTarget,
                               D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
                               D3D12_BARRIER_ACCESS_RENDER_TARGET,
                               D3D12_BARRIER_ACCESS_NO_ACCESS,
                               D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                               D3D12_BARRIER_LAYOUT_PRESENT));
        }

        m_profiler.endFrame(m_commandList.Get(), frameIndex);

        R4D_HR(m_commandList->Close(), "ID3D12GraphicsCommandList::Close");
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_queue->ExecuteCommandLists(1, lists);

        if (!m_headless)
        {
            const UINT syncInterval = m_vsync ? 1u : 0u;
            const UINT presentFlags =
                (!m_vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
            const HRESULT presented = m_swapChain->Present(syncInterval, presentFlags);
            if (presented == DXGI_ERROR_DEVICE_REMOVED ||
                presented == DXGI_ERROR_DEVICE_RESET)
            {
                logDeviceRemovedDiagnostics(m_device.Get(),
                                            m_device->GetDeviceRemovedReason());
                return hresultError(ErrorCode::ApiCallFailed, presented,
                                    "IDXGISwapChain::Present");
            }
            if (FAILED(presented))
            {
                return hresultError(ErrorCode::ApiCallFailed, presented,
                                    "IDXGISwapChain::Present");
            }
        }

        m_lastRenderedBackBuffer = backBufferIndex;
        m_hasRenderedFrame = true;

        // The value this slot must reach before reuse.
        frame.fenceValue = m_nextFenceValue;
        R4D_HR(m_queue->Signal(m_fence.Get(), m_nextFenceValue), "ID3D12CommandQueue::Signal");
        ++m_nextFenceValue;
        ++m_frameCounter;

        m_stats.cpuFrameMs = cpuDelta * 1000.0;
        m_stats.cpuFrameAverageMs = m_frameTimer.averageMilliseconds();
        m_stats.drawCalls = drawCalls;
        m_stats.dispatches = 0;
        m_stats.triangles = triangles;
        m_stats.renderResolution = m_size;
        m_stats.frameIndex = m_frameCounter;

        return Status::Ok();
    }

    Status D3D12Renderer::resize(UVec2 newSize)
    {
        if (newSize == m_size)
        {
            return Status::Ok();
        }

        if (newSize.x == 0 || newSize.y == 0)
        {
            // ResizeBuffers rejects 0x0.
            m_size = newSize;
            R4D_DEBUG("Swapchain: minimised, rendering suspended");
            return Status::Ok();
        }

        const bool wasMinimised = (m_size.x == 0 || m_size.y == 0);

        // Drain every in-flight frame before releasing the render target.
        waitForGpuIdle();

        if (m_headless)
        {
            m_offscreenTarget.Reset();
            R4D_TRY(createOffscreenTarget(newSize));
            R4D_INFO("Offscreen target: resized to {}x{}", m_size.x, m_size.y);
            return Status::Ok();
        }

        releaseBackBuffers();

        UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (m_allowTearing)
        {
            flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        const HRESULT hr = m_swapChain->ResizeBuffers(kBackBufferCount, newSize.x, newSize.y,
                                                      kBackBufferFormat, flags);
        if (FAILED(hr))
        {
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                logDeviceRemovedDiagnostics(m_device.Get(), m_device->GetDeviceRemovedReason());
            }
            return hresultError(ErrorCode::ApiCallFailed, hr, "IDXGISwapChain::ResizeBuffers");
        }

        m_size = newSize;
        R4D_TRY(acquireBackBuffers());

        // Nothing has been drawn into the new buffers yet.
        m_hasRenderedFrame = false;

        R4D_INFO("Swapchain: resized to {}x{}{}", m_size.x, m_size.y,
                 wasMinimised ? " (restored from minimised)" : "");
        return Status::Ok();
    }

    Result<FrameCapture> D3D12Renderer::captureFrame()
    {
        if (!m_hasRenderedFrame)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "captureFrame called before any frame was rendered into the "
                             "current render target");
        }
        if (m_size.x == 0 || m_size.y == 0)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "captureFrame called with a zero-sized render target");
        }

        // Draining first also makes any allocator safe to reuse below.
        waitForGpuIdle();

        ID3D12Resource* source = currentRenderTarget(m_lastRenderedBackBuffer);
        const D3D12_BARRIER_LAYOUT restingLayout =
            m_headless ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : D3D12_BARRIER_LAYOUT_PRESENT;

        const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &footprint, &rowCount,
                                        &rowBytes, &totalBytes);

        if (!m_captureReadback || m_captureReadbackBytes < totalBytes)
        {
            m_captureReadback.Reset();
            const auto readbackHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
            const auto desc = bufferDesc(totalBytes);
            R4D_HR(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                                     &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&m_captureReadback)),
                   "CreateCommittedResource (capture readback)");
            m_captureReadback->SetName(L"Radiance4D Capture Readback");
            m_captureReadbackBytes = totalBytes;
        }

        auto& frame = m_frames[0];
        R4D_HR(frame.commandAllocator->Reset(), "Reset (capture allocator)");
        R4D_HR(m_commandList->Reset(frame.commandAllocator.Get(), nullptr),
               "Reset (capture command list)");

        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(source, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COPY,
                           D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                           restingLayout, D3D12_BARRIER_LAYOUT_COPY_SOURCE));

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m_captureReadback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION origin{};
        origin.pResource = source;
        origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        origin.SubresourceIndex = 0;

        m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);

        // Put it back for the next frame.
        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(source, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE,
                           D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                           D3D12_BARRIER_LAYOUT_COPY_SOURCE, restingLayout));

        R4D_HR(m_commandList->Close(), "Close (capture command list)");
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_queue->ExecuteCommandLists(1, lists);

        const UINT64 captureFence = m_nextFenceValue++;
        R4D_HR(m_queue->Signal(m_fence.Get(), captureFence), "Signal (capture fence)");
        frame.fenceValue = captureFence;
        R4D_TRY(waitForFenceValue(captureFence));

        void* mapped = nullptr;
        const D3D12_RANGE readEverything{0, static_cast<SIZE_T>(totalBytes)};
        R4D_HR(m_captureReadback->Map(0, &readEverything, &mapped),
               "Map (capture readback)");

        FrameCapture capture;
        capture.width = static_cast<std::uint32_t>(sourceDesc.Width);
        capture.height = static_cast<std::uint32_t>(sourceDesc.Height);
        capture.rgba.resize(static_cast<std::size_t>(capture.width) * capture.height * 4u);

        // The footprint pitch is padded, so copy row by row.
        const auto* sourceBytes =
            static_cast<const std::byte*>(mapped) + footprint.Offset;
        for (std::uint32_t y = 0; y < capture.height; ++y)
        {
            std::memcpy(capture.rgba.data() + static_cast<std::size_t>(y) * capture.width * 4u,
                        sourceBytes + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                        static_cast<std::size_t>(capture.width) * 4u);
        }

        const D3D12_RANGE wroteNothing{0, 0};
        m_captureReadback->Unmap(0, &wroteNothing);

        R4D_DEBUG("Capture: {}x{} read back ({} bytes, row pitch {})", capture.width,
                  capture.height, capture.rgba.size(), footprint.Footprint.RowPitch);
        return capture;
    }

} // namespace r4d::d3d12
