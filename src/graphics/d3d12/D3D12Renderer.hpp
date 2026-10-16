// D3D12Renderer.hpp
#pragma once

#include "D3D12Adapter.hpp"
#include "D3D12Profiler.hpp"
#include "D3D12Validation.hpp"

#include <radiance4d/core/RollingAverage.hpp>
#include <radiance4d/core/Time.hpp>
#include <radiance4d/graphics/Renderer.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace r4d::d3d12
{

    class D3D12Renderer final : public Renderer
    {
    public:
        static Result<std::unique_ptr<Renderer>> create(const RendererDesc& desc);

        ~D3D12Renderer() override;

        void setCamera(const CameraParams& camera) override { m_camera = camera; }
        void setRenderSettings(const RenderSettings& settings) override
        {
            m_settings = settings;
        }
        const RenderSettings& renderSettings() const override { return m_settings; }
        void setUiCallback(std::function<void()> callback) override
        {
            m_uiCallback = std::move(callback);
        }
        Status renderFrame() override;
        Status resize(UVec2 newSize) override;
        void waitForGpuIdle() override;
        Result<FrameCapture> captureFrame() override;
        bool headless() const override { return m_headless; }

        BackendType backend() const override { return BackendType::D3D12; }
        const AdapterInfo& adapterInfo() const override { return m_adapterInfo; }
        const GpuCapabilities& capabilities() const override { return m_capabilities; }
        const GpuStats& stats() const override { return m_stats; }

        void setVsync(bool enabled) override { m_vsync = enabled; }
        bool vsync() const override { return m_vsync; }

        std::uint64_t validationErrorCount() const override
        {
            return m_validation.errorCount();
        }

    private:
        D3D12Renderer() = default;

        // Back-buffer index and frame-in-flight index are separate things.
        // GetCurrentBackBufferIndex() picks the render target only; allocators,
        // fence slots and constant buffers go by frameCounter % kFramesInFlight.
        // The counts are kept unequal so conflating them breaks loudly.
        static constexpr UINT kBackBufferCount = 3;
        static constexpr UINT kFramesInFlight = 2;
        static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

        // Never indexed by back-buffer index.
        struct FrameContext
        {
            ComPtr<ID3D12CommandAllocator> commandAllocator;
            ComPtr<ID3D12Resource> constantBuffer;   // upload heap, persistently mapped
            std::byte* constantBufferCpu = nullptr;
            // The allocator may not be reset until the fence reaches this.
            UINT64 fenceValue = 0;
        };

        Status initialise(const RendererDesc& desc);
        Status createDeviceAndQueue(const RendererDesc& desc);
        Status createSwapChain(void* windowHandle, UVec2 size);
        Status createFrameResources();
        Status createPipeline(const std::filesystem::path& shaderDirectory);
        Status uploadModel(const Model& model) override;
        Status uploadGaussians(const GaussianCloud& cloud) override;
        Status uploadTextures(const Model& model);
        Result<ComPtr<ID3D12Resource>> createTexture(const ImageRgba8& image, bool srgb,
                                                     ID3D12GraphicsCommandList7* commandList,
                                                     std::vector<ComPtr<ID3D12Resource>>& staging);
        Status createRtvHeap();
        Status acquireBackBuffers();
        void releaseBackBuffers();

        Status createOffscreenTarget(UVec2 size);
        Status createDepthBuffer(UVec2 size);
        Status createShadowMap();
        Status createShadowPipeline(const std::filesystem::path& shaderDirectory);
        Status createSplatPipeline(const std::filesystem::path& shaderDirectory);
        Status createSortPipelines(const std::filesystem::path& shaderDirectory);
        void sortSplats(UINT frameIndex);
        void renderSplats(UINT frameIndex, std::uint32_t& drawCalls,
                          std::uint32_t& triangles);
        void renderShadowPass();
        Mat4 lightViewProjection() const;
        Status initialiseUi(void* windowHandle);
        void shutdownUi();
        void renderUi();

        ID3D12Resource* currentRenderTarget(UINT backBufferIndex) const;
        D3D12_CPU_DESCRIPTOR_HANDLE currentRenderTargetView(UINT backBufferIndex) const;

        Status waitForFenceValue(UINT64 value);

        Result<std::vector<std::byte>> loadShaderBlob(
            const std::filesystem::path& shaderDirectory, std::string_view name) const;

        ComPtr<IDXGIFactory6> m_factory;
        // Device10 for CreateCommandList1 and CreateCommittedResource3.
        ComPtr<ID3D12Device10> m_device;
        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<IDXGISwapChain3> m_swapChain;

        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        ComPtr<ID3D12Resource> m_depthBuffer;
        ComPtr<ID3D12Resource> m_shadowMap;
        ComPtr<ID3D12PipelineState> m_shadowPipeline;
        Bounds m_sceneBounds;
        static constexpr UINT kShadowMapSize = 2048;
        ComPtr<ID3D12DescriptorHeap> m_srvHeap;
        ComPtr<ID3D12DescriptorHeap> m_uiHeap;
        std::function<void()> m_uiCallback;
        bool m_uiReady = false;
        UINT m_srvDescriptorSize = 0;
        std::vector<ComPtr<ID3D12Resource>> m_textures;
        UINT m_rtvDescriptorSize = 0;
        std::array<ComPtr<ID3D12Resource>, kBackBufferCount> m_backBuffers{};

        std::array<FrameContext, kFramesInFlight> m_frames{};
        ComPtr<ID3D12GraphicsCommandList7> m_commandList;

        ComPtr<ID3D12Fence> m_fence;
        UINT64 m_nextFenceValue = 1;
        HANDLE m_fenceEvent = nullptr;
        HANDLE m_frameLatencyWaitable = nullptr;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipelineState;

        ComPtr<ID3D12Resource> m_vertexBuffer;
        ComPtr<ID3D12Resource> m_indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};
        std::vector<Primitive> m_primitives;
        std::vector<Material> m_materials;

        ComPtr<ID3D12RootSignature> m_splatRootSignature;
        ComPtr<ID3D12PipelineState> m_splatPipeline;
        ComPtr<ID3D12Resource> m_gaussianBuffer;
        // One per frame in flight: the depth order is rewritten every frame.
        // Device-local now that compute writes it, and also the sort's values-A
        // buffer -- four passes ping-pong and land the result back in here.
        std::array<ComPtr<ID3D12Resource>, kFramesInFlight> m_gaussianOrder{};
        std::uint32_t m_gaussianCount = 0;

        // The stages of one radix-sort pass, in dispatch order. The key build
        // runs once per frame rather than once per pass.
        enum class SortStage : std::size_t
        {
            Cull,
            Histogram,
            ScanRows,
            ScanTotals,
            Scatter,
            Count
        };

        // Scratch for the sort, one set per frame in flight so that a frame
        // never writes what the previous one is still reading. The per-slot
        // fence wait in renderFrame is what makes that safe without a barrier.
        struct SortBuffers
        {
            ComPtr<ID3D12Resource> keysA;
            ComPtr<ID3D12Resource> keysB;
            ComPtr<ID3D12Resource> valuesB;
            ComPtr<ID3D12Resource> histogram;
            ComPtr<ID3D12Resource> digitBase;
            // One projection per splat, written by the cull pass and read
            // by the vertex shader.
            ComPtr<ID3D12Resource> projections;
        };
        std::array<SortBuffers, kFramesInFlight> m_sortBuffers{};
        ComPtr<ID3D12RootSignature> m_sortRootSignature;
        std::array<ComPtr<ID3D12PipelineState>,
                   static_cast<std::size_t>(SortStage::Count)>
            m_sortPipelines{};
        std::uint32_t m_sortTileCount = 0;
        // m_gaussianCount rounded up to a whole number of tiles.
        std::uint32_t m_sortPaddedCount = 0;

        ComPtr<ID3D12Resource> m_offscreenTarget;
        ComPtr<ID3D12Resource> m_captureReadback;
        UINT64 m_captureReadbackBytes = 0;

        RenderSettings m_settings;
        CameraParams m_camera;
        AdapterInfo m_adapterInfo;
        GpuCapabilities m_capabilities;
        ValidationSink m_validation;
        D3D12Profiler m_profiler;
        GpuStats m_stats;
        FrameTimer m_frameTimer;
        RollingAverage<FrameTimer::kHistorySize> m_gpuTimeHistory;

        UVec2 m_size{0, 0};
        std::uint64_t m_frameCounter = 0;
        // Non-zero overrides the wall clock, for reproducible captures.
        double m_fixedTimeStep = 0.0;
        bool m_headless = false;
        bool m_vsync = true;
        bool m_allowTearing = false;
        bool m_debugLayerActive = false;
        // After Present the current index is the next buffer to draw, not the
        // one just drawn, so a capture needs this.
        UINT m_lastRenderedBackBuffer = 0;
        bool m_hasRenderedFrame = false;
    };

} // namespace r4d::d3d12
