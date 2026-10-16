// D3D12Renderer.cpp
#include "D3D12Renderer.hpp"

#include <radiance4d/core/FileSystem.hpp>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstring>

namespace r4d::d3d12
{
    namespace
    {
        constexpr UINT kLightCount = 3;

        struct Light
        {
            Vec3 direction;
            float intensity;
            Vec3 colour;
            float padding;
        };

        struct FrameConstants
        {
            Mat4 viewProjection;
            Mat4 lightViewProjection;
            Mat4 view;
            Vec3 cameraPosition;
            float timeSeconds;
            Vec3 ambient;
            float exposure;
            float shadowTexelSize;
            float shadowPadding[3];
            Vec2 splatViewport;
            Vec2 splatFocal;
            float splatNearPlane;
            float splatPadding[3];
            Light lights[kLightCount];
        };

        struct MaterialConstants
        {
            Vec4 baseColour;
            Vec3 emissive;
            float metallic;
            float roughness;
            float padding[3];
        };

        // Each material owns a contiguous triple of SRVs: base colour,
        // metallic-roughness, normal. Missing maps point at a default texture.
        // base colour, metallic-roughness, normal, environment, irradiance, shadow
        constexpr UINT kTexturesPerMaterial = 6;

        static_assert(sizeof(MaterialConstants) % sizeof(UINT32) == 0);
        constexpr UINT kMaterialRootConstants = sizeof(MaterialConstants) / sizeof(UINT32);

        constexpr UINT64 kConstantBufferAlignment = 256;

        // Closer than this the Jacobian is meaningless, so the splat is dropped.
        constexpr float kSplatNearPlane = 0.05f;

        // All four must match SplatSort.hlsli.
        constexpr UINT kSortGroupSize = 256;
        constexpr UINT kSortDigitBits = 8;
        constexpr UINT kSortBuckets = 1u << kSortDigitBits;
        constexpr UINT kSortPasses = 32 / kSortDigitBits;
        static_assert(kSortBuckets == kSortGroupSize,
                      "SplatSortScanTotals scans every digit's total in one group");

        // Mirrors SortConstants in SplatSort.hlsli.
        struct SortConstants
        {
            std::uint32_t count;
            std::uint32_t splatCount;
            std::uint32_t tileCount;
            std::uint32_t shift;
            std::uint32_t sourceIsB;
            std::uint32_t pairCapacity;
            std::uint32_t pairTotalSlot;
            std::uint32_t pairPass;
        };
        static_assert(sizeof(SortConstants) == 32);
        constexpr UINT kSortRootConstants = sizeof(SortConstants) / sizeof(UINT32);

        // One root descriptor per buffer the sort binds: both halves of the
        // key and value ping-pong, the per-tile histograms and the digit bases.
        constexpr UINT kSortUavCount = 15;

        // Must match R4D_TILE_SIZE in SplatTiles.hlsli.
        constexpr UINT kSplatTileSize = 16;
        // Enough tile ranges for any viewport this renderer will be resized to.
        constexpr UINT kMaxTiles = ((7680 + kSplatTileSize - 1) / kSplatTileSize) *
                                   ((4320 + kSplatTileSize - 1) / kSplatTileSize);
        // How many (tile, splat) pairs are allowed per splat on average. A
        // frame that needs more drops the overflow, which is reported.
        constexpr UINT kPairsPerSplat = 8;
        constexpr DXGI_FORMAT kSplatColourFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        // Mirrors SplatProjection in Splat.hlsli. GPU-only, so only its size
        // matters here, but the fields are named so the two can be compared.
        struct SplatProjection
        {
            float screen[2];
            float radius;
            float depth;
            float conic[3];
            float opacity;
            float colour[3];
            float padding;
        };
        static_assert(sizeof(SplatProjection) == 48);


        // Must match the optimised clear value the render target is created with.
        constexpr float kClearColour[4] = {0.043f, 0.047f, 0.063f, 1.0f};
        constexpr float kClearDepth = 1.0f;

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

        // Orders every preceding access against every following one. The sort
        // stages each read what the one before wrote, across six buffers, which
        // is exactly what a global barrier says in one line.
        void submitGlobalBarrier(ID3D12GraphicsCommandList7* commandList,
                                 D3D12_BARRIER_SYNC syncBefore,
                                 D3D12_BARRIER_SYNC syncAfter,
                                 D3D12_BARRIER_ACCESS accessBefore,
                                 D3D12_BARRIER_ACCESS accessAfter)
        {
            D3D12_GLOBAL_BARRIER barrier{};
            barrier.SyncBefore = syncBefore;
            barrier.SyncAfter = syncAfter;
            barrier.AccessBefore = accessBefore;
            barrier.AccessAfter = accessAfter;

            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_GLOBAL;
            group.NumBarriers = 1;
            group.pGlobalBarriers = &barrier;
            commandList->Barrier(1, &group);
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
            desc.CullMode = D3D12_CULL_MODE_BACK;
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

        shutdownUi();

        m_textures.clear();
        m_srvHeap.Reset();
        m_shadowPipeline.Reset();
        m_splatPipeline.Reset();
        m_splatColour.Reset();
        m_splatHeap.Reset();
        m_splatRootSignature.Reset();
        m_sortPipelines = {};
        m_sortRootSignature.Reset();
        m_sortBuffers = {};
        m_gaussianBuffer.Reset();
        m_gaussianOrder = {};
        m_shadowMap.Reset();
        m_depthBuffer.Reset();
        m_dsvHeap.Reset();
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

        R4D_TRY(createDepthBuffer(m_size));
        R4D_TRY(createSplatTargets(m_size));
        R4D_TRY(createShadowMap());
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
        R4D_TRY(createShadowPipeline(shaderDirectory));
        R4D_TRY(createSplatPipeline(shaderDirectory));
        R4D_TRY(createSortPipelines(shaderDirectory));

        if (!m_headless)
        {
            R4D_TRY(initialiseUi(desc.windowHandle));
        }

        // Missing timestamp support is not fatal; stats report them as invalid.
        m_profiler.setPassScopesEnabled(desc.passTimings);
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

    Status D3D12Renderer::initialiseUi(void* windowHandle)
    {
        // ImGui needs its own shader-visible descriptor for the font atlas.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        R4D_HR(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_uiHeap)),
               "CreateDescriptorHeap (ImGui)");
        m_uiHeap->SetName(L"Radiance4D ImGui Heap");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui::GetIO().IniFilename = nullptr;

        if (!ImGui_ImplWin32_Init(windowHandle))
        {
            return makeError(ErrorCode::ApiCallFailed, "ImGui_ImplWin32_Init failed");
        }

        ImGui_ImplDX12_InitInfo info{};
        info.Device = m_device.Get();
        info.CommandQueue = m_queue.Get();
        info.NumFramesInFlight = static_cast<int>(kFramesInFlight);
        info.RTVFormat = kBackBufferFormat;
        info.DSVFormat = kDepthFormat;
        info.SrvDescriptorHeap = m_uiHeap.Get();
        info.LegacySingleSrvCpuDescriptor = m_uiHeap->GetCPUDescriptorHandleForHeapStart();
        info.LegacySingleSrvGpuDescriptor = m_uiHeap->GetGPUDescriptorHandleForHeapStart();

        if (!ImGui_ImplDX12_Init(&info))
        {
            return makeError(ErrorCode::ApiCallFailed, "ImGui_ImplDX12_Init failed");
        }

        m_uiReady = true;
        R4D_INFO("UI: Dear ImGui {} ready", IMGUI_VERSION);
        return Status::Ok();
    }

    void D3D12Renderer::shutdownUi()
    {
        if (!m_uiReady)
        {
            return;
        }
        m_uiReady = false;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_uiHeap.Reset();
    }

    void D3D12Renderer::renderUi()
    {
        if (!m_uiReady)
        {
            return;
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (m_uiCallback)
        {
            m_uiCallback();
        }

        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = {m_uiHeap.Get()};
        m_commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
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

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 2;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        R4D_HR(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)),
               "ID3D12Device::CreateDescriptorHeap (DSV)");
        m_dsvHeap->SetName(L"Radiance4D DSV Heap");
        return Status::Ok();
    }

    Status D3D12Renderer::createDepthBuffer(UVec2 size)
    {
        m_depthBuffer.Reset();

        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size.x;
        desc.Height = size.y;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        // Typeless, because the tile rasteriser reads the same texture as
        // R32_FLOAT while the mesh pass writes it as D32_FLOAT.
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = kClearDepth;

        const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        R4D_HR(m_device->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                                                  &clear, nullptr, 0, nullptr,
                                                  IID_PPV_ARGS(&m_depthBuffer)),
               "CreateCommittedResource3 (depth buffer)");
        m_depthBuffer->SetName(L"Radiance4D Depth Buffer");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc,
                                         m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
        return Status::Ok();
    }

    Status D3D12Renderer::createShadowMap()
    {
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kShadowMapSize;
        desc.Height = kShadowMapSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        // Typeless so the same resource can be a depth target and a shader
        // resource.
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = kClearDepth;

        const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // Created in the layout the frame loop expects to start from, so the
        // first frame needs no special case.
        R4D_HR(m_device->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                                                  &clear, nullptr, 0, nullptr,
                                                  IID_PPV_ARGS(&m_shadowMap)),
               "CreateCommittedResource3 (shadow map)");
        m_shadowMap->SetName(L"Radiance4D Shadow Map");

        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, dsv);
        return Status::Ok();
    }

    Mat4 D3D12Renderer::lightViewProjection() const
    {
        // Orthographic box fitted to the scene bounds along the key light.
        const Vec3 direction = glm::normalize(Vec3{-0.4f, 0.8f, -0.45f});
        const Vec3 centre = m_sceneBounds.valid() ? m_sceneBounds.centre() : Vec3{0.0f};
        const float radius = m_sceneBounds.valid() ? std::max(m_sceneBounds.radius(), 0.01f)
                                                   : 1.0f;

        const Vec3 eye = centre + direction * (radius * 2.0f);
        const Vec3 up = std::abs(direction.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f}
                                                      : Vec3{0.0f, 1.0f, 0.0f};
        const Mat4 view = glm::lookAt(eye, centre, up);
        const Mat4 projection =
            glm::ortho(-radius, radius, -radius, radius, 0.05f, radius * 4.5f);
        return projection * view;
    }

    void D3D12Renderer::renderShadowPass()
    {
        if (m_primitives.empty() || !m_shadowPipeline || !m_shadowMap)
        {
            return;
        }

        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(m_shadowMap.Get(), D3D12_BARRIER_SYNC_NONE,
                           D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_NO_ACCESS,
                           D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                           D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE));

        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0,
                                             nullptr);

        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                                static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
        m_commandList->RSSetViewports(1, &viewport);
        D3D12_RECT scissor{0, 0, static_cast<LONG>(kShadowMapSize),
                           static_cast<LONG>(kShadowMapSize)};
        m_commandList->RSSetScissorRects(1, &scissor);

        m_commandList->SetPipelineState(m_shadowPipeline.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        m_commandList->IASetIndexBuffer(&m_indexBufferView);

        for (const Primitive& primitive : m_primitives)
        {
            m_commandList->DrawIndexedInstanced(primitive.indexCount, 1, primitive.firstIndex,
                                                primitive.vertexOffset, 0);
        }

        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(m_shadowMap.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                           D3D12_BARRIER_SYNC_PIXEL_SHADING,
                           D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                           D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                           D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
    }

    Status D3D12Renderer::createShadowPipeline(const std::filesystem::path& shaderDirectory)
    {
        auto vertexShader = loadShaderBlob(shaderDirectory, "Shadow.vs.dxil");
        if (!vertexShader)
        {
            return vertexShader.error();
        }

        const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tangent),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS.pShaderBytecode = vertexShader.value().data();
        psoDesc.VS.BytecodeLength = vertexShader.value().size();
        psoDesc.BlendState = opaqueBlendDesc();
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = defaultRasterizerDesc();
        // Constant and slope-scaled bias in the rasteriser rather than only in the
        // shader, which is what keeps large flat surfaces from acne.
        psoDesc.RasterizerState.DepthBias = 120;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.InputLayout.pInputElementDescs = inputLayout;
        psoDesc.InputLayout.NumElements = static_cast<UINT>(std::size(inputLayout));
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 0;
        psoDesc.DSVFormat = kDepthFormat;
        psoDesc.SampleDesc.Count = 1;

        R4D_HR(m_device->CreateGraphicsPipelineState(&psoDesc,
                                                     IID_PPV_ARGS(&m_shadowPipeline)),
               "CreateGraphicsPipelineState (shadow)");
        m_shadowPipeline->SetName(L"Radiance4D Shadow PSO");
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

    Status D3D12Renderer::createSplatTargets(UVec2 size)
    {
        m_splatColour.Reset();

        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size.x;
        desc.Height = size.y;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kSplatColourFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        R4D_HR(m_device->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                                                  nullptr, nullptr, 0, nullptr,
                                                  IID_PPV_ARGS(&m_splatColour)),
               "CreateCommittedResource3 (splat colour)");
        m_splatColour->SetName(L"Radiance4D Splat Colour");

        if (!m_splatHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 3;   // depth SRV, colour UAV, colour SRV
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            R4D_HR(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_splatHeap)),
                   "CreateDescriptorHeap (splat)");
            m_splatHeap->SetName(L"Radiance4D Splat Heap");
            m_splatDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_splatHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_depthBuffer.Get(), &depthSrv, handle);
        handle.ptr += m_splatDescriptorSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = kSplatColourFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(m_splatColour.Get(), nullptr, &uav, handle);
        handle.ptr += m_splatDescriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC colourSrv{};
        colourSrv.Format = kSplatColourFormat;
        colourSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        colourSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        colourSrv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_splatColour.Get(), &colourSrv, handle);

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
        auto vertexShader = loadShaderBlob(shaderDirectory, "Pbr.vs.dxil");
        if (!vertexShader)
        {
            return vertexShader.error();
        }
        auto pixelShader = loadShaderBlob(shaderDirectory, "Pbr.ps.dxil");
        if (!pixelShader)
        {
            return pixelShader.error();
        }

        // One root CBV at b0; materials in M2 will need more.
        D3D12_ROOT_PARAMETER1 rootParameters[3]{};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.Flags =
            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        // Material is small and changes per draw, so it goes in root constants
        // rather than through a descriptor table.
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].Constants.ShaderRegister = 1;
        rootParameters[1].Constants.RegisterSpace = 0;
        rootParameters[1].Constants.Num32BitValues = kMaterialRootConstants;

        D3D12_DESCRIPTOR_RANGE1 textureRange{};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = kTexturesPerMaterial;
        textureRange.BaseShaderRegister = 0;
        textureRange.RegisterSpace = 0;
        // Not DATA_STATIC: the shadow map in this range changes layout every
        // frame, which that flag promises will not happen.
        textureRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        textureRange.OffsetInDescriptorsFromTableStart = 0;

        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[2].DescriptorTable.pDescriptorRanges = &textureRange;

        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        D3D12_STATIC_SAMPLER_DESC& sampler = samplers[0];
        sampler.Filter = D3D12_FILTER_ANISOTROPIC;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxAnisotropy = 8;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC& environmentSampler = samplers[1];
        environmentSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        environmentSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        environmentSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        environmentSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        environmentSampler.MaxAnisotropy = 1;
        environmentSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        environmentSampler.MinLOD = 0.0f;
        environmentSampler.MaxLOD = D3D12_FLOAT32_MAX;
        environmentSampler.ShaderRegister = 1;
        environmentSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC& shadowSampler = samplers[2];
        shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.MaxAnisotropy = 1;
        shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        // White border: anything outside the map is lit, not shadowed.
        shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        shadowSampler.MinLOD = 0.0f;
        shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
        shadowSampler.ShaderRegister = 2;
        shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = static_cast<UINT>(std::size(rootParameters));
        rootDesc.Desc_1_1.pParameters = rootParameters;
        rootDesc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
        rootDesc.Desc_1_1.pStaticSamplers = samplers;
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
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tangent),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv),
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
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.InputLayout.pInputElementDescs = inputLayout;
        psoDesc.InputLayout.NumElements = static_cast<UINT>(std::size(inputLayout));
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = kBackBufferFormat;
        psoDesc.DSVFormat = kDepthFormat;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        R4D_HR(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)),
               "ID3D12Device::CreateGraphicsPipelineState");
        m_pipelineState->SetName(L"Radiance4D PBR PSO");

        return Status::Ok();
    }

    Result<ComPtr<ID3D12Resource>> D3D12Renderer::createTexture(
        const ImageRgba8& image, bool srgb, ID3D12GraphicsCommandList7* commandList,
        std::vector<ComPtr<ID3D12Resource>>& staging)
    {
        const std::vector<ImageRgba8> levels = generateMipChain(image);
        const auto mipCount = static_cast<UINT16>(levels.size());

        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = image.width;
        desc.Height = image.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = mipCount;
        desc.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        R4D_HR(m_device->CreateCommittedResource3(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr,
                                                  nullptr, 0, nullptr,
                                                  IID_PPV_ARGS(&texture)),
               "CreateCommittedResource3 (texture)");

        const D3D12_RESOURCE_DESC uploadSource = texture->GetDesc();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
        std::vector<UINT> rowCounts(mipCount);
        std::vector<UINT64> rowBytes(mipCount);
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&uploadSource, 0, mipCount, 0, footprints.data(),
                                        rowCounts.data(), rowBytes.data(), &totalBytes);

        const auto uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto stagingDesc = bufferDesc(totalBytes);
        ComPtr<ID3D12Resource> upload;
        R4D_HR(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                 &stagingDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload)),
               "CreateCommittedResource (texture upload)");

        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        R4D_HR(upload->Map(0, &noRead, &mapped), "Map (texture upload)");
        for (UINT16 level = 0; level < mipCount; ++level)
        {
            auto* destination = static_cast<std::byte*>(mapped) + footprints[level].Offset;
            const ImageRgba8& source = levels[level];
            for (UINT row = 0; row < rowCounts[level]; ++row)
            {
                std::memcpy(destination + static_cast<std::size_t>(row) *
                                              footprints[level].Footprint.RowPitch,
                            source.rgba.data() + static_cast<std::size_t>(row) *
                                                     source.rowPitch(),
                            static_cast<std::size_t>(rowBytes[level]));
            }
        }
        upload->Unmap(0, nullptr);

        for (UINT16 level = 0; level < mipCount; ++level)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = level;

            D3D12_TEXTURE_COPY_LOCATION origin{};
            origin.pResource = upload.Get();
            origin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            origin.PlacedFootprint = footprints[level];

            commandList->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
        }

        D3D12_TEXTURE_BARRIER barrier{};
        barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
        barrier.SyncAfter = D3D12_BARRIER_SYNC_PIXEL_SHADING;
        barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
        barrier.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
        barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        barrier.pResource = texture.Get();
        barrier.Subresources = D3D12_BARRIER_SUBRESOURCE_RANGE{0, mipCount, 0, 1, 0, 1};
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        group.NumBarriers = 1;
        group.pTextureBarriers = &barrier;
        commandList->Barrier(1, &group);

        staging.push_back(upload);
        return texture;
    }

    Status D3D12Renderer::uploadTextures(const Model& model)
    {
        m_textures.clear();

        // Slots 0..2 are the defaults every material falls back to: white,
        // white (metallic-roughness factors apply) and a flat normal.
        ImageRgba8 white;
        white.width = 1;
        white.height = 1;
        white.rgba = {255, 255, 255, 255};

        ImageRgba8 flatNormal = white;
        flatNormal.rgba = {128, 128, 255, 255};

        const auto descriptorCount =
            static_cast<UINT>((model.materials.size() + 1) * kTexturesPerMaterial);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = descriptorCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        R4D_HR(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)),
               "CreateDescriptorHeap (SRV)");
        m_srvHeap->SetName(L"Radiance4D SRV Heap");
        m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto& frame = m_frames[0];
        R4D_HR(frame.commandAllocator->Reset(), "Reset (texture allocator)");
        R4D_HR(m_commandList->Reset(frame.commandAllocator.Get(), nullptr),
               "Reset (texture command list)");

        std::vector<ComPtr<ID3D12Resource>> staging;

        auto makeTexture = [&](const ImageRgba8& image,
                               bool srgb) -> Result<ComPtr<ID3D12Resource>> {
            return createTexture(image, srgb, m_commandList.Get(), staging);
        };

        auto whiteSrgb = makeTexture(white, true);
        if (!whiteSrgb) return whiteSrgb.error();
        auto whiteLinear = makeTexture(white, false);
        if (!whiteLinear) return whiteLinear.error();
        auto normalDefault = makeTexture(flatNormal, false);
        if (!normalDefault) return normalDefault.error();

        const ImageRgba8& skyImage =
            model.environment.image.empty() ? white : model.environment.image;
        const ImageRgba8& irradianceImage =
            model.irradiance.image.empty() ? white : model.irradiance.image;
        auto sky = makeTexture(skyImage, true);
        if (!sky) return sky.error();
        auto irradiance = makeTexture(irradianceImage, true);
        if (!irradiance) return irradiance.error();

        m_textures.push_back(whiteSrgb.value());
        m_textures.push_back(whiteLinear.value());
        m_textures.push_back(normalDefault.value());
        m_textures.push_back(sky.value());
        m_textures.push_back(irradiance.value());

        std::vector<ComPtr<ID3D12Resource>> modelTextures;
        modelTextures.reserve(model.textures.size());
        for (const TextureImage& source : model.textures)
        {
            auto created = makeTexture(source.image, source.srgb);
            if (!created) return created.error();
            modelTextures.push_back(created.value());
            m_textures.push_back(created.value());
        }

        R4D_HR(m_commandList->Close(), "Close (texture command list)");
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        const UINT64 fenceValue = m_nextFenceValue++;
        R4D_HR(m_queue->Signal(m_fence.Get(), fenceValue), "Signal (texture fence)");
        frame.fenceValue = fenceValue;
        R4D_TRY(waitForFenceValue(fenceValue));

        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto writeSrv = [&](ID3D12Resource* resource) {
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = desc.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = desc.MipLevels;
            m_device->CreateShaderResourceView(resource, &srv, handle);
            handle.ptr += m_srvDescriptorSize;
        };

        // The shadow map is typeless, so it needs its own view description.
        const auto writeShadowSrv = [&]() {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(m_shadowMap.Get(), &srv, handle);
            handle.ptr += m_srvDescriptorSize;
        };

        writeSrv(whiteSrgb.value().Get());
        writeSrv(whiteLinear.value().Get());
        writeSrv(normalDefault.value().Get());
        writeSrv(sky.value().Get());
        writeSrv(irradiance.value().Get());
        writeShadowSrv();

        for (const Material& material : model.materials)
        {
            const auto pick = [&](std::int32_t index,
                                  ID3D12Resource* fallback) -> ID3D12Resource* {
                if (index >= 0 && static_cast<std::size_t>(index) < modelTextures.size())
                {
                    return modelTextures[static_cast<std::size_t>(index)].Get();
                }
                return fallback;
            };
            writeSrv(pick(material.baseColourTexture, whiteSrgb.value().Get()));
            writeSrv(pick(material.metallicRoughnessTexture, whiteLinear.value().Get()));
            writeSrv(pick(material.normalTexture, normalDefault.value().Get()));
            writeSrv(sky.value().Get());
            writeSrv(irradiance.value().Get());
            writeShadowSrv();
        }

        R4D_INFO("Textures: {} uploaded, {} descriptors", model.textures.size(),
                 descriptorCount);
        return Status::Ok();
    }

    Status D3D12Renderer::uploadModel(const Model& model)
    {
        if (model.empty() || model.vertices.empty() || model.indices.empty())
        {
            return makeError(ErrorCode::InvalidArgument, "uploadModel called with no geometry");
        }

        waitForGpuIdle();
        m_vertexBuffer.Reset();
        m_indexBuffer.Reset();

        const UINT64 vertexBytes = model.vertices.size() * sizeof(Vertex);
        const UINT64 indexBytes = model.indices.size() * sizeof(std::uint32_t);

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
            std::memcpy(mapped, model.vertices.data(), static_cast<std::size_t>(vertexBytes));
            vertexUpload->Unmap(0, nullptr);

            R4D_HR(indexUpload->Map(0, &noRead, &mapped), "Map (index upload)");
            std::memcpy(mapped, model.indices.data(), static_cast<std::size_t>(indexBytes));
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
        frame.fenceValue = uploadFence;

        m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
        m_vertexBufferView.StrideInBytes = sizeof(Vertex);

        m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
        m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

        m_primitives = model.primitives;
        m_materials = model.materials;
        m_sceneBounds = model.bounds;

        R4D_TRY(uploadTextures(model));

        R4D_INFO("Geometry: {} vertices, {} triangles, {} primitives",
                 model.vertices.size(), model.triangleCount(), model.primitives.size());
        return Status::Ok();
    }

    Status D3D12Renderer::uploadGaussians(const GaussianCloud& cloud)
    {
        waitForGpuIdle();
        m_gaussianBuffer.Reset();
        m_gaussianOrder = {};
        m_sortBuffers = {};
        m_gaussianCount = 0;
        m_sortTileCount = 0;
        m_sortPaddedCount = 0;

        if (cloud.empty())
        {
            return Status::Ok();
        }

        const auto count = static_cast<std::uint32_t>(cloud.count());
        const UINT64 splatBytes = static_cast<UINT64>(count) * sizeof(Gaussian);

        const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        const auto uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto splatDesc = bufferDesc(splatBytes);

        R4D_HR(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                 &splatDesc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&m_gaussianBuffer)),
               "CreateCommittedResource (gaussian buffer)");
        m_gaussianBuffer->SetName(L"Radiance4D Gaussian Buffer");

        ComPtr<ID3D12Resource> upload;
        R4D_HR(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                 &splatDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload)),
               "CreateCommittedResource (gaussian upload)");
        {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{0, 0};
            R4D_HR(upload->Map(0, &noRead, &mapped), "Map (gaussian upload)");
            std::memcpy(mapped, cloud.gaussians.data(),
                        static_cast<std::size_t>(splatBytes));
            upload->Unmap(0, nullptr);
        }

        // Every kernel but the key build runs without a bounds check, so the
        // sort buffers hold a whole number of tiles and the key build fills the
        // tail with the largest possible key.
        m_sortTileCount = (count + kSortGroupSize - 1) / kSortGroupSize;
        m_sortPaddedCount = m_sortTileCount * kSortGroupSize;

        // The pairs are bounded rather than grown: a frame needing more drops
        // the overflow, which uploadGaussians reports as a limit.
        m_pairCapacity =
            ((count * kPairsPerSplat + kSortGroupSize - 1) / kSortGroupSize) * kSortGroupSize;
        m_pairTileCount = m_pairCapacity / kSortGroupSize;

        // Digit-major, one row of per-tile counts per digit. The tile sort runs
        // over the pairs, which is the larger of the two.
        const UINT64 histogramEntries =
            static_cast<UINT64>(kSortBuckets) * std::max(m_sortTileCount, m_pairTileCount);

        const auto createSortBuffer = [&](UINT64 entries, const wchar_t* name,
                                          ComPtr<ID3D12Resource>& out) -> Status {
            auto desc = bufferDesc(entries * sizeof(std::uint32_t));
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            R4D_HR(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                     &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&out)),
                   "CreateCommittedResource (splat sort buffer)");
            out->SetName(name);
            return Status::Ok();
        };

        for (UINT i = 0; i < kFramesInFlight; ++i)
        {
            SortBuffers& sort = m_sortBuffers[i];
            // The order the vertex shader reads is the sort's values-A buffer:
            // with an even number of passes the result lands back in it.
            R4D_TRY(createSortBuffer(m_sortPaddedCount, L"Radiance4D Gaussian Order",
                                     m_gaussianOrder[i]));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, L"Radiance4D Splat Sort Keys A",
                                     sort.keysA));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, L"Radiance4D Splat Sort Keys B",
                                     sort.keysB));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, L"Radiance4D Splat Sort Values B",
                                     sort.valuesB));
            R4D_TRY(createSortBuffer(histogramEntries, L"Radiance4D Splat Sort Histogram",
                                     sort.histogram));
            R4D_TRY(createSortBuffer(kSortBuckets, L"Radiance4D Splat Sort Digit Bases",
                                     sort.digitBase));

            R4D_TRY(createSortBuffer(m_sortPaddedCount, L"Radiance4D Splat Tile Counts",
                                     sort.tileCounts));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, L"Radiance4D Splat Tile Offsets",
                                     sort.tileOffsets));
            // One slot per group, plus one for the grand total.
            R4D_TRY(createSortBuffer(m_sortTileCount + 1, L"Radiance4D Splat Group Sums",
                                     sort.groupSums));
            R4D_TRY(createSortBuffer(m_pairCapacity, L"Radiance4D Splat Pair Tiles A",
                                     sort.pairTileA));
            R4D_TRY(createSortBuffer(m_pairCapacity, L"Radiance4D Splat Pair Tiles B",
                                     sort.pairTileB));
            R4D_TRY(createSortBuffer(m_pairCapacity, L"Radiance4D Splat Pair Splats A",
                                     sort.pairSplatA));
            R4D_TRY(createSortBuffer(m_pairCapacity, L"Radiance4D Splat Pair Splats B",
                                     sort.pairSplatB));
            R4D_TRY(createSortBuffer(kMaxTiles * 2, L"Radiance4D Splat Tile Ranges",
                                     sort.tileRanges));

            auto readbackDesc = bufferDesc(sizeof(std::uint32_t));
            const auto readbackHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
            R4D_HR(m_device->CreateCommittedResource(
                       &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                       IID_PPV_ARGS(&sort.pairTotalReadback)),
                   "CreateCommittedResource (splat pair total readback)");
            sort.pairTotalReadback->SetName(L"Radiance4D Splat Pair Total");

            // One projection per real splat; the sort's padding never reaches
            // the draw, so it needs none.
            auto projectionDesc =
                bufferDesc(static_cast<UINT64>(count) * sizeof(SplatProjection));
            projectionDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            R4D_HR(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                     &projectionDesc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&sort.projections)),
                   "CreateCommittedResource (splat projections)");
            sort.projections->SetName(L"Radiance4D Splat Projections");

        }

        auto& frame = m_frames[0];
        R4D_HR(frame.commandAllocator->Reset(), "Reset (gaussian upload allocator)");
        R4D_HR(m_commandList->Reset(frame.commandAllocator.Get(), nullptr),
               "Reset (gaussian upload command list)");
        m_commandList->CopyBufferRegion(m_gaussianBuffer.Get(), 0, upload.Get(), 0,
                                        splatBytes);

        D3D12_BUFFER_BARRIER barrier{};
        barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
        // The sort reads the positions out of the same buffer the vertex
        // shader rasterises from, so both stages are in the sync scope.
        barrier.SyncAfter =
            D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
        barrier.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        barrier.pResource = m_gaussianBuffer.Get();
        barrier.Offset = 0;
        barrier.Size = UINT64_MAX;

        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_BUFFER;
        group.NumBarriers = 1;
        group.pBufferBarriers = &barrier;
        m_commandList->Barrier(1, &group);

        R4D_HR(m_commandList->Close(), "Close (gaussian upload command list)");
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_queue->ExecuteCommandLists(1, lists);

        const UINT64 uploadFence = m_nextFenceValue++;
        R4D_HR(m_queue->Signal(m_fence.Get(), uploadFence), "Signal (gaussian fence)");
        R4D_TRY(waitForFenceValue(uploadFence));
        frame.fenceValue = uploadFence;

        m_gaussianCount = count;

        constexpr double kMiB = 1024.0 * 1024.0;
        R4D_INFO("Gaussians: {} splats uploaded ({:.1f} MiB), sorted on the GPU in {} "
                 "tiles ({:.1f} MiB of sort buffers, {:.1f} MiB of pair buffers for "
                 "{} pairs)",
                 count, static_cast<double>(splatBytes) / kMiB, m_sortTileCount,
                 static_cast<double>(sortBufferBytes()) / kMiB,
                 static_cast<double>(pairBufferBytes()) / kMiB, m_pairCapacity);
        return Status::Ok();
    }

    // Everything the splat path holds whose size is fixed by the splat count.
    UINT64 D3D12Renderer::sortBufferBytes() const
    {
        if (m_gaussianCount == 0)
        {
            return 0;
        }
        // Per frame slot: the order, both halves of the key ping-pong and the
        // spare value half, the per-splat tile counts and their prefix sum, the
        // scan's group sums, the digit bases and the per-tile ranges.
        const UINT64 entries = static_cast<UINT64>(6) * m_sortPaddedCount +
                               m_sortTileCount + 1 + kSortBuckets + kMaxTiles * 2;
        return (entries * sizeof(std::uint32_t) +
                static_cast<UINT64>(m_gaussianCount) * sizeof(SplatProjection)) *
               kFramesInFlight;
    }

    // The four pair buffers and the histogram the tile sort shares with the
    // depth sort, which between them are most of what the splat path costs.
    UINT64 D3D12Renderer::pairBufferBytes() const
    {
        if (m_gaussianCount == 0)
        {
            return 0;
        }
        const UINT64 entries =
            static_cast<UINT64>(4) * m_pairCapacity +
            static_cast<UINT64>(kSortBuckets) * std::max(m_sortTileCount, m_pairTileCount);
        return entries * sizeof(std::uint32_t) * kFramesInFlight;
    }

    void D3D12Renderer::sortSplats(UINT frameIndex)
    {
        if (m_gaussianCount == 0 || !m_sortRootSignature)
        {
            return;
        }

        const FrameContext& frame = m_frames[frameIndex];
        SortBuffers& sort = m_sortBuffers[frameIndex];

        // This slot's buffers were last touched two frames ago, and renderFrame
        // has already waited on that frame's fence, so nothing here needs a
        // barrier against the previous frame's reads.
        m_commandList->SetComputeRootSignature(m_sortRootSignature.Get());
        m_commandList->SetComputeRootConstantBufferView(
            0, frame.constantBuffer->GetGPUVirtualAddress());
        m_commandList->SetComputeRootShaderResourceView(
            2, m_gaussianBuffer->GetGPUVirtualAddress());

        ID3D12Resource* const buffers[kSortUavCount] = {
            sort.keysA.Get(),       sort.keysB.Get(),
            m_gaussianOrder[frameIndex].Get(), sort.valuesB.Get(),
            sort.histogram.Get(),   sort.digitBase.Get(),
            sort.projections.Get(), sort.tileCounts.Get(),
            sort.tileOffsets.Get(), sort.groupSums.Get(),
            sort.pairTileA.Get(),   sort.pairTileB.Get(),
            sort.pairSplatA.Get(),  sort.pairSplatB.Get(),
            sort.tileRanges.Get()};
        for (UINT i = 0; i < kSortUavCount; ++i)
        {
            m_commandList->SetComputeRootUnorderedAccessView(
                3 + i, buffers[i]->GetGPUVirtualAddress());
        }

        SortConstants constants{};
        constants.count = m_sortPaddedCount;
        constants.splatCount = m_gaussianCount;
        constants.tileCount = m_sortTileCount;
        constants.pairCapacity = m_pairCapacity;
        // The binning scan leaves the pair total one past the per-group sums.
        constants.pairTotalSlot = m_sortTileCount;

        const auto dispatch = [&](SortStage stage, UINT groups) {
            m_commandList->SetPipelineState(
                m_sortPipelines[static_cast<std::size_t>(stage)].Get());
            m_commandList->SetComputeRoot32BitConstants(1, kSortRootConstants, &constants,
                                                        0);
            m_commandList->Dispatch(groups, 1, 1);
            submitGlobalBarrier(m_commandList.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        };

        {
            D3D12PassScope scope(m_profiler, m_commandList.Get(), frameIndex,
                                 GpuPass::SplatCull);
            dispatch(SortStage::Cull, m_sortTileCount);
        }

        {
            D3D12PassScope scope(m_profiler, m_commandList.Get(), frameIndex,
                                 GpuPass::SplatDepthSort);
            for (UINT pass = 0; pass < kSortPasses; ++pass)
            {
                constants.shift = pass * kSortDigitBits;
                // Pass 0 reads A, and an even pass count brings the result home.
                constants.sourceIsB = pass & 1u;
                dispatch(SortStage::Histogram, m_sortTileCount);
                dispatch(SortStage::ScanRows, kSortBuckets);
                dispatch(SortStage::ScanTotals, 1);
                dispatch(SortStage::Scatter, m_sortTileCount);
            }
        }

        // Tile binning. The pairs are generated by walking the depth order, so
        // they start out sorted by depth; the tile sort below is stable, which
        // leaves every tile's slice still in depth order.
        {
            D3D12PassScope scope(m_profiler, m_commandList.Get(), frameIndex,
                                 GpuPass::SplatBinning);
            dispatch(SortStage::BinCount, m_sortTileCount);
            dispatch(SortStage::ScanGroups, m_sortTileCount);
            dispatch(SortStage::ScanGroupSums, 1);
            dispatch(SortStage::ScanAdd, m_sortTileCount);
            dispatch(SortStage::Bin, m_pairTileCount);

            // The scan left the pair total one past the per-group sums.
            D3D12_BUFFER_BARRIER toCopy{};
            toCopy.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            toCopy.SyncAfter = D3D12_BARRIER_SYNC_COPY;
            toCopy.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            toCopy.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
            toCopy.pResource = sort.groupSums.Get();
            toCopy.Offset = 0;
            toCopy.Size = UINT64_MAX;

            D3D12_BARRIER_GROUP copyGroup{};
            copyGroup.Type = D3D12_BARRIER_TYPE_BUFFER;
            copyGroup.NumBarriers = 1;
            copyGroup.pBufferBarriers = &toCopy;
            m_commandList->Barrier(1, &copyGroup);

            m_commandList->CopyBufferRegion(
                sort.pairTotalReadback.Get(), 0, sort.groupSums.Get(),
                static_cast<UINT64>(m_sortTileCount) * sizeof(std::uint32_t),
                sizeof(std::uint32_t));

            toCopy.SyncBefore = D3D12_BARRIER_SYNC_COPY;
            toCopy.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            toCopy.AccessBefore = D3D12_BARRIER_ACCESS_COPY_SOURCE;
            toCopy.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            m_commandList->Barrier(1, &copyGroup);
        }

        // The same radix sort, pointed at the pairs: two passes cover a tile id
        // up to 16 bits, which is more than any viewport this renderer takes.
        // The per-tile ranges the rasteriser walks come out of the same scope.
        {
            D3D12PassScope scope(m_profiler, m_commandList.Get(), frameIndex,
                                 GpuPass::SplatTileSort);

            ID3D12Resource* const pairBuffers[4] = {sort.pairTileA.Get(),
                                                    sort.pairTileB.Get(),
                                                    sort.pairSplatA.Get(),
                                                    sort.pairSplatB.Get()};
            for (UINT i = 0; i < 4; ++i)
            {
                m_commandList->SetComputeRootUnorderedAccessView(
                    3 + i, pairBuffers[i]->GetGPUVirtualAddress());
            }

            constants.count = m_pairCapacity;
            constants.tileCount = m_pairTileCount;
            // From here the kernels size themselves from the pair total rather
            // than from the capacity, which the ranges pass below also wants.
            constants.pairPass = 1;
            for (UINT pass = 0; pass < 2; ++pass)
            {
                constants.shift = pass * kSortDigitBits;
                constants.sourceIsB = pass & 1u;
                dispatch(SortStage::Histogram, m_pairTileCount);
                dispatch(SortStage::ScanRows, kSortBuckets);
                dispatch(SortStage::ScanTotals, 1);
                dispatch(SortStage::Scatter, m_pairTileCount);
            }

            for (UINT i = 0; i < 4; ++i)
            {
                m_commandList->SetComputeRootUnorderedAccessView(
                    3 + i, buffers[i]->GetGPUVirtualAddress());
            }

            const UINT tilesX = (m_size.x + kSplatTileSize - 1) / kSplatTileSize;
            const UINT tilesY = (m_size.y + kSplatTileSize - 1) / kSplatTileSize;
            dispatch(SortStage::TileRangesReset,
                     (tilesX * tilesY + kSortGroupSize - 1) / kSortGroupSize);
            dispatch(SortStage::TileRanges, m_pairTileCount);

            D3D12_BUFFER_BARRIER barriers[2]{};
            barriers[0].SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            barriers[0].SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            barriers[0].AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            barriers[0].AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            barriers[0].pResource = sort.tileRanges.Get();
            barriers[0].Offset = 0;
            barriers[0].Size = UINT64_MAX;

            barriers[1] = barriers[0];
            barriers[1].pResource = sort.pairSplatA.Get();

            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = static_cast<UINT32>(std::size(barriers));
            group.pBufferBarriers = barriers;
            m_commandList->Barrier(1, &group);
        }
    }

    void D3D12Renderer::rasteriseSplats(UINT frameIndex)
    {
        if (m_gaussianCount == 0 || !m_sortRootSignature)
        {
            return;
        }

        const SortBuffers& sort = m_sortBuffers[frameIndex];
        const FrameContext& frame = m_frames[frameIndex];

        // Open to the end of the function, so the handover barriers either side
        // of the dispatch are charged to the rasteriser that needs them.
        D3D12PassScope scope(m_profiler, m_commandList.Get(), frameIndex,
                             GpuPass::SplatRaster);

        // The mesh pass wrote the depth it is about to read.
        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(m_depthBuffer.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                           D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                           D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                           D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));

        ID3D12DescriptorHeap* heaps[] = {m_splatHeap.Get()};
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetComputeRootSignature(m_sortRootSignature.Get());
        m_commandList->SetComputeRootConstantBufferView(
            0, frame.constantBuffer->GetGPUVirtualAddress());
        m_commandList->SetComputeRootShaderResourceView(
            2, m_gaussianBuffer->GetGPUVirtualAddress());

        ID3D12Resource* const buffers[kSortUavCount] = {
            sort.keysA.Get(),       sort.keysB.Get(),
            m_gaussianOrder[frameIndex].Get(), sort.valuesB.Get(),
            sort.histogram.Get(),   sort.digitBase.Get(),
            sort.projections.Get(), sort.tileCounts.Get(),
            sort.tileOffsets.Get(), sort.groupSums.Get(),
            sort.pairTileA.Get(),   sort.pairTileB.Get(),
            sort.pairSplatA.Get(),  sort.pairSplatB.Get(),
            sort.tileRanges.Get()};
        for (UINT i = 0; i < kSortUavCount; ++i)
        {
            m_commandList->SetComputeRootUnorderedAccessView(
                3 + i, buffers[i]->GetGPUVirtualAddress());
        }
        m_commandList->SetComputeRootDescriptorTable(
            3 + kSortUavCount, m_splatHeap->GetGPUDescriptorHandleForHeapStart());

        SortConstants constants{};
        constants.count = m_sortPaddedCount;
        constants.splatCount = m_gaussianCount;
        constants.tileCount = m_sortTileCount;
        constants.pairCapacity = m_pairCapacity;
        constants.pairTotalSlot = m_sortTileCount;
        m_commandList->SetComputeRoot32BitConstants(1, kSortRootConstants, &constants, 0);

        m_commandList->SetPipelineState(
            m_sortPipelines[static_cast<std::size_t>(SortStage::Raster)].Get());
        const UINT tilesX = (m_size.x + kSplatTileSize - 1) / kSplatTileSize;
        const UINT tilesY = (m_size.y + kSplatTileSize - 1) / kSplatTileSize;
        m_commandList->Dispatch(tilesX, tilesY, 1);

        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(m_depthBuffer.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                           D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                           D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                           D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE));

        // The composite reads what the rasteriser just wrote.
        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(m_splatColour.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_SYNC_PIXEL_SHADING,
                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                           D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                           D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
    }

    void D3D12Renderer::renderSplats(UINT frameIndex, std::uint32_t& drawCalls,
                                     std::uint32_t& triangles)
    {
        if (m_gaussianCount == 0 || !m_splatPipeline)
        {
            return;
        }

        D3D12PassScope scope(m_profiler, m_commandList.Get(), frameIndex,
                             GpuPass::SplatComposite);

        // The tile rasteriser already composited the splats among themselves;
        // this puts that image over the mesh.
        ID3D12DescriptorHeap* heaps[] = {m_splatHeap.Get()};
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetPipelineState(m_splatPipeline.Get());
        m_commandList->SetGraphicsRootSignature(m_splatRootSignature.Get());

        D3D12_GPU_DESCRIPTOR_HANDLE colour =
            m_splatHeap->GetGPUDescriptorHandleForHeapStart();
        colour.ptr += 2ull * m_splatDescriptorSize;   // depth SRV, colour UAV, colour SRV
        m_commandList->SetGraphicsRootDescriptorTable(0, colour);

        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(3, 1, 0, 0);

        submitTextureBarrier(
            m_commandList.Get(),
            textureBarrier(m_splatColour.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                           D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                           D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS));

        ++drawCalls;
        triangles += 1;
    }

    Status D3D12Renderer::createSortPipelines(const std::filesystem::path& shaderDirectory)
    {
        // Buffers only, so root descriptors are enough here too: no heap, no
        // table. 24 DWORDs of a 64-DWORD budget.
        D3D12_ROOT_PARAMETER1 rootParameters[4 + kSortUavCount]{};

        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.Flags =
            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        // The per-pass constants change between dispatches, so they ride in the
        // root signature rather than in a buffer that would need re-uploading.
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[1].Constants.ShaderRegister = 1;
        rootParameters[1].Constants.Num32BitValues = kSortRootConstants;

        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[2].Descriptor.ShaderRegister = 0;
        rootParameters[2].Descriptor.Flags =
            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        for (UINT i = 0; i < kSortUavCount; ++i)
        {
            D3D12_ROOT_PARAMETER1& parameter = rootParameters[3 + i];
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameter.Descriptor.ShaderRegister = i;
            parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        }

        // Only the rasteriser uses these two, but one root signature across
        // every splat kernel keeps the binding code in one place.
        D3D12_DESCRIPTOR_RANGE1 textureRanges[2]{};
        textureRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRanges[0].NumDescriptors = 1;
        textureRanges[0].BaseShaderRegister = 3;   // t3, the mesh depth
        textureRanges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        textureRanges[0].OffsetInDescriptorsFromTableStart = 0;

        textureRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        textureRanges[1].NumDescriptors = 1;
        textureRanges[1].BaseShaderRegister = 15;   // u15, the splat colour
        textureRanges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        textureRanges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER1& tableParameter = rootParameters[3 + kSortUavCount];
        tableParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        tableParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        tableParameter.DescriptorTable.NumDescriptorRanges =
            static_cast<UINT>(std::size(textureRanges));
        tableParameter.DescriptorTable.pDescriptorRanges = textureRanges;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = static_cast<UINT>(std::size(rootParameters));
        rootDesc.Desc_1_1.pParameters = rootParameters;
        rootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

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
                                std::format("SerializeVersionedRootSignature (splat "
                                            "sort): {}",
                                            detail));
        }

        R4D_HR(m_device->CreateRootSignature(0, serialised->GetBufferPointer(),
                                             serialised->GetBufferSize(),
                                             IID_PPV_ARGS(&m_sortRootSignature)),
               "CreateRootSignature (splat sort)");
        m_sortRootSignature->SetName(L"Radiance4D Splat Sort Root Signature");

        // Indexed by SortStage, so the order of these must match the enum.
        static constexpr std::string_view kStageShaders[] = {
            "SplatCull.cs.dxil",          "SplatSortHistogram.cs.dxil",
            "SplatSortScanRows.cs.dxil",  "SplatSortScanTotals.cs.dxil",
            "SplatSortScatter.cs.dxil",   "SplatBinCount.cs.dxil",
            "SplatScanGroups.cs.dxil",    "SplatScanGroupSums.cs.dxil",
            "SplatScanAdd.cs.dxil",       "SplatBin.cs.dxil",
            "SplatTileRangesReset.cs.dxil", "SplatTileRanges.cs.dxil",
            "SplatRaster.cs.dxil"};
        static_assert(std::size(kStageShaders) ==
                      static_cast<std::size_t>(SortStage::Count));

        for (std::size_t i = 0; i < std::size(kStageShaders); ++i)
        {
            auto blob = loadShaderBlob(shaderDirectory, kStageShaders[i]);
            if (!blob)
            {
                return blob.error();
            }

            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = m_sortRootSignature.Get();
            desc.CS.pShaderBytecode = blob.value().data();
            desc.CS.BytecodeLength = blob.value().size();
            R4D_HR(m_device->CreateComputePipelineState(&desc,
                                                        IID_PPV_ARGS(&m_sortPipelines[i])),
                   "CreateComputePipelineState (splat sort)");
        }

        return Status::Ok();
    }

    Status D3D12Renderer::createSplatPipeline(const std::filesystem::path& shaderDirectory)
    {
        auto vertexShader = loadShaderBlob(shaderDirectory, "SplatComposite.vs.dxil");
        if (!vertexShader)
        {
            return vertexShader.error();
        }
        auto pixelShader = loadShaderBlob(shaderDirectory, "SplatComposite.ps.dxil");
        if (!pixelShader)
        {
            return pixelShader.error();
        }

        // One texture, read by Load, so there is no sampler and no constants.
        D3D12_DESCRIPTOR_RANGE1 range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        D3D12_ROOT_PARAMETER1 rootParameters[1]{};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[0].DescriptorTable.pDescriptorRanges = &range;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = static_cast<UINT>(std::size(rootParameters));
        rootDesc.Desc_1_1.pParameters = rootParameters;
        rootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

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
                                std::format("SerializeVersionedRootSignature (splat "
                                            "composite): {}",
                                            detail));
        }

        R4D_HR(m_device->CreateRootSignature(0, serialised->GetBufferPointer(),
                                             serialised->GetBufferSize(),
                                             IID_PPV_ARGS(&m_splatRootSignature)),
               "CreateRootSignature (splat composite)");
        m_splatRootSignature->SetName(L"Radiance4D Splat Composite Root Signature");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_splatRootSignature.Get();
        psoDesc.VS.pShaderBytecode = vertexShader.value().data();
        psoDesc.VS.BytecodeLength = vertexShader.value().size();
        psoDesc.PS.pShaderBytecode = pixelShader.value().data();
        psoDesc.PS.BytecodeLength = pixelShader.value().size();

        // Premultiplied, which is what the rasteriser wrote.
        D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = defaultRasterizerDesc();
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        // The depth test happened per splat in the rasteriser.
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = kBackBufferFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        R4D_HR(m_device->CreateGraphicsPipelineState(&psoDesc,
                                                     IID_PPV_ARGS(&m_splatPipeline)),
               "CreateGraphicsPipelineState (splat composite)");
        m_splatPipeline->SetName(L"Radiance4D Splat Composite PSO");
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

        // The same wait means last time's pair total has landed.
        if (m_gaussianCount != 0 && !m_pairOverflowReported)
        {
            const SortBuffers& sort = m_sortBuffers[frameIndex];
            if (sort.pairTotalReadback)
            {
                std::uint32_t total = 0;
                void* mapped = nullptr;
                const D3D12_RANGE range{0, sizeof(total)};
                if (SUCCEEDED(sort.pairTotalReadback->Map(0, &range, &mapped)))
                {
                    std::memcpy(&total, mapped, sizeof(total));
                    const D3D12_RANGE noWrite{0, 0};
                    sort.pairTotalReadback->Unmap(0, &noWrite);
                }
                if (total > m_pairCapacity)
                {
                    m_pairOverflowReported = true;
                    R4D_WARN("Splats: tile binning needs {} pairs but holds {}; the "
                             "overflow is dropped and this view is missing splats",
                             total, m_pairCapacity);
                }
            }
        }

        // The wait above also means this slot's timestamps are resolved.
        double gpuMilliseconds = 0.0;
        if (m_profiler.readFrameTimeMs(frameIndex, gpuMilliseconds))
        {
            m_gpuTimeHistory.add(gpuMilliseconds);
            m_stats.gpuFrameMs = gpuMilliseconds;
            m_stats.gpuFrameAverageMs = m_gpuTimeHistory.average();
            m_stats.gpuTimingsValid = true;
        }

        // All or nothing: the scopes are only resolved when every one of them was
        // bracketed, which needs the splat path to have run.
        bool passTimesValid = m_profiler.passScopesEnabled();
        for (std::size_t pass = 0; passTimesValid && pass < kGpuPassCount; ++pass)
        {
            double passMilliseconds = 0.0;
            if (!m_profiler.readPassTimeMs(frameIndex, static_cast<GpuPass>(pass),
                                           passMilliseconds))
            {
                passTimesValid = false;
                break;
            }
            m_gpuPassHistory[pass].add(passMilliseconds);
            m_stats.gpuPassMs[pass] = passMilliseconds;
            m_stats.gpuPassAverageMs[pass] = m_gpuPassHistory[pass].average();
        }
        m_stats.gpuPassTimingsValid = passTimesValid;

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
            FrameConstants constants{};
            constants.viewProjection = m_camera.projection * m_camera.view;
            constants.lightViewProjection = lightViewProjection();
            constants.view = m_camera.view;
            constants.splatViewport = Vec2{static_cast<float>(m_size.x),
                                           static_cast<float>(m_size.y)};
            // Focal length in pixels, recovered from the projection, which is
            // what the covariance Jacobian needs.
            constants.splatFocal =
                Vec2{0.5f * static_cast<float>(m_size.x) * m_camera.projection[0][0],
                     0.5f * static_cast<float>(m_size.y) * m_camera.projection[1][1]};
            constants.splatNearPlane = kSplatNearPlane;
            constants.shadowTexelSize = 1.0f / static_cast<float>(kShadowMapSize);
            constants.cameraPosition = m_camera.position;
            constants.timeSeconds = timeSeconds;
            constants.ambient = Vec3{m_settings.ambientIntensity, m_settings.ambientIntensity,
                                     m_settings.ambientIntensity};
            constants.exposure = m_settings.exposure;

            constants.lights[0].direction = glm::normalize(Vec3{-0.4f, 0.8f, -0.45f});
            constants.lights[0].colour = Vec3{1.0f, 0.96f, 0.9f};
            constants.lights[0].intensity = 2.4f;

            constants.lights[1].direction = glm::normalize(Vec3{0.75f, 0.25f, 0.3f});
            constants.lights[1].colour = Vec3{0.35f, 0.52f, 0.95f};
            constants.lights[1].intensity = 0.9f;

            constants.lights[2].direction = glm::normalize(Vec3{0.1f, -0.6f, 0.75f});
            constants.lights[2].colour = Vec3{0.95f, 0.45f, 0.3f};
            constants.lights[2].intensity = 0.5f;
            std::memcpy(frame.constantBufferCpu, &constants, sizeof(constants));
        }

        R4D_HR(frame.commandAllocator->Reset(), "ID3D12CommandAllocator::Reset");
        R4D_HR(m_commandList->Reset(frame.commandAllocator.Get(), m_pipelineState.Get()),
               "ID3D12GraphicsCommandList::Reset");

        m_profiler.beginFrame(m_commandList.Get(), frameIndex);

        // Before anything binds a render target: the splat order for this frame
        // is produced by compute, and the vertex shader reads it further down.
        sortSplats(frameIndex);

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        if (m_srvHeap)
        {
            ID3D12DescriptorHeap* shadowHeaps[] = {m_srvHeap.Get()};
            m_commandList->SetDescriptorHeaps(1, shadowHeaps);
        }
        m_commandList->SetGraphicsRootConstantBufferView(
            0, frame.constantBuffer->GetGPUVirtualAddress());
        renderShadowPass();
        m_commandList->SetPipelineState(m_pipelineState.Get());

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

        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        m_commandList->ClearRenderTargetView(rtv, kClearColour, 0, nullptr);
        m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0,
                                             0, nullptr);

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
        if (m_srvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
            m_commandList->SetDescriptorHeaps(1, heaps);
        }
        m_commandList->SetGraphicsRootConstantBufferView(
            0, frame.constantBuffer->GetGPUVirtualAddress());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);

        m_commandList->IASetIndexBuffer(&m_indexBufferView);

        std::uint32_t drawCalls = 0;
        std::uint32_t triangles = 0;

        for (const Primitive& primitive : m_primitives)
        {
            const Material& material =
                primitive.materialIndex < m_materials.size()
                    ? m_materials[primitive.materialIndex]
                    : m_materials.front();

            MaterialConstants constants{};
            constants.baseColour = material.baseColour;
            constants.emissive = material.emissive;
            constants.metallic = material.metallic;
            constants.roughness = material.roughness;
            m_commandList->SetGraphicsRoot32BitConstants(1, kMaterialRootConstants,
                                                         &constants, 0);

            // Defaults occupy the first triple, so materials start one along.
            D3D12_GPU_DESCRIPTOR_HANDLE table =
                m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(primitive.materialIndex + 1) *
                         kTexturesPerMaterial * m_srvDescriptorSize;
            m_commandList->SetGraphicsRootDescriptorTable(2, table);

            m_commandList->DrawIndexedInstanced(primitive.indexCount, 1,
                                                primitive.firstIndex,
                                                primitive.vertexOffset, 0);
            ++drawCalls;
            triangles += primitive.indexCount / 3;
        }

        if (m_gaussianCount != 0)
        {
            // Compute cannot run with the depth buffer bound for writing, and
            // the rasteriser reads it, so the mesh pass hands it over here.
            m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            rasteriseSplats(frameIndex);
            m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            renderSplats(frameIndex, drawCalls, triangles);
        }

        renderUi();

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
            R4D_TRY(createDepthBuffer(newSize));
            R4D_TRY(createSplatTargets(newSize));
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
        R4D_TRY(createDepthBuffer(newSize));
        R4D_TRY(createSplatTargets(newSize));

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
