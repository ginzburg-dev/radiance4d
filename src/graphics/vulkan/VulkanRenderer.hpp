// VulkanRenderer.hpp
#pragma once

#include "VulkanDevice.hpp"

#include <radiance4d/core/RollingAverage.hpp>
#include <radiance4d/core/Time.hpp>
#include <radiance4d/graphics/Renderer.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace r4d::vulkan
{

    class VulkanRenderer final : public Renderer
    {
    public:
        static Result<std::unique_ptr<Renderer>> create(const RendererDesc& desc);

        ~VulkanRenderer() override;

        Status uploadModel(const Model& model) override;
        Status uploadGaussians(const GaussianCloud& cloud) override;
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

        BackendType backend() const override { return BackendType::Vulkan; }
        const AdapterInfo& adapterInfo() const override { return m_context.adapterInfo; }
        const GpuCapabilities& capabilities() const override
        {
            return m_context.capabilities;
        }
        const GpuStats& stats() const override { return m_stats; }

        void setVsync(bool enabled) override;
        bool vsync() const override { return m_vsync; }

        std::uint64_t validationErrorCount() const override
        {
            return m_validation.errorCount();
        }

    private:
        VulkanRenderer() = default;

        static constexpr std::uint32_t kFramesInFlight = 2;
        static constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;
        static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

        struct Buffer
        {
            VkBuffer handle = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* mapped = nullptr;
            VkDeviceSize size = 0;
        };

        struct Image
        {
            VkImage handle = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            std::uint32_t mipLevels = 1;
        };

        struct Frame
        {
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkSemaphore acquired = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE;
        };

        Status initialise(const RendererDesc& desc);
        Status createSurface(void* windowHandle);
        Status createSwapchain(UVec2 size);
        void destroySwapchain();
        Status createDepthImage(UVec2 size);
        Status createShadowMap();
        Status createShadowPipeline(const std::filesystem::path& shaderDirectory);
        void renderShadowPass(VkCommandBuffer commandBuffer);
        Mat4 lightViewProjection() const;
        Status createOffscreenTarget(UVec2 size);
        Status createFrameResources();
        Status createDescriptorLayout();
        Status createPipeline(const std::filesystem::path& shaderDirectory);
        Status createSplatPipeline(const std::filesystem::path& shaderDirectory);
        Status createSplatDescriptors();
        Status createSortPipelines(const std::filesystem::path& shaderDirectory);
        Status createSortDescriptors();
        VkDeviceSize sortBufferBytes() const;
        VkDeviceSize pairBufferBytes() const;
        void destroySplatBuffers();
        void sortSplats(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);
        void rasteriseSplats(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);
        Status createSplatTargets(UVec2 size);
        void updateSplatImageDescriptors();
        void renderSplats(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                          std::uint32_t& drawCalls, std::uint32_t& triangles);

        static std::uint32_t passQueryIndex(std::uint32_t frameIndex, GpuPass pass)
        {
            return frameIndex * kQueriesPerSlot + kFrameQueries +
                   2 * static_cast<std::uint32_t>(pass);
        }

        void beginPassScope(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                            GpuPass pass);
        void endPassScope(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                          GpuPass pass);

        // Brackets one pass. The destructor closes the scope, so an early return
        // cannot leave a begin unmatched.
        class PassScope
        {
        public:
            PassScope(VulkanRenderer& renderer, VkCommandBuffer commandBuffer,
                      std::uint32_t frameIndex, GpuPass pass)
                : m_renderer(renderer), m_commandBuffer(commandBuffer),
                  m_frameIndex(frameIndex), m_pass(pass)
            {
                m_renderer.beginPassScope(m_commandBuffer, m_frameIndex, m_pass);
            }

            ~PassScope()
            {
                m_renderer.endPassScope(m_commandBuffer, m_frameIndex, m_pass);
            }

            PassScope(const PassScope&) = delete;
            PassScope& operator=(const PassScope&) = delete;

        private:
            VulkanRenderer& m_renderer;
            VkCommandBuffer m_commandBuffer;
            std::uint32_t m_frameIndex;
            GpuPass m_pass;
        };

        Status createDescriptorSets(const Model& model);
        Status createTimestampPool();
        Status initialiseUi(void* windowHandle);
        void shutdownUi();
        void renderUi(VkCommandBuffer commandBuffer);

        Result<Buffer> createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags properties, const char* name);
        void destroyBuffer(Buffer& buffer);
        Result<Image> createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                                  VkImageUsageFlags usage, std::uint32_t mipLevels,
                                  VkImageAspectFlags aspect);
        void destroyImage(Image& image);
        Result<Image> createTexture(const ImageRgba8& source, bool srgb,
                                    VkCommandBuffer commandBuffer,
                                    std::vector<Buffer>& staging);

        Result<VkShaderModule> loadShaderModule(const std::filesystem::path& directory,
                                                std::string_view name) const;
        Result<VkCommandBuffer> beginOneShot(VkCommandPool pool);
        Status endOneShot(VkCommandPool pool, VkCommandBuffer commandBuffer);

        VkImage currentColourImage() const;
        VkImageView currentColourView() const;

        DeviceContext m_context;
        ValidationSink m_validation;

        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkFormat m_swapchainFormat = kColourFormat;
        std::vector<VkImage> m_swapchainImages;
        std::vector<VkImageView> m_swapchainViews;
        // Signalled per swapchain IMAGE, not per frame in flight: presentation
        // completion follows the image, and with more images than frame slots a
        // per-slot semaphore can still be in use by an earlier present.
        std::vector<VkSemaphore> m_renderFinished;
        std::uint32_t m_imageIndex = 0;

        Image m_offscreen;
        Image m_depth;
        Image m_shadowMap;
        VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
        VkSampler m_shadowSampler = VK_NULL_HANDLE;
        Bounds m_sceneBounds;
        static constexpr std::uint32_t kShadowMapSize = 2048;
        bool m_shadowMapInitialised = false;

        std::array<Frame, kFramesInFlight> m_frames{};
        VkCommandPool m_uploadPool = VK_NULL_HANDLE;

        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> m_materialSets;
        VkSampler m_sampler = VK_NULL_HANDLE;
        VkSampler m_environmentSampler = VK_NULL_HANDLE;

        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;

        // The splat pass reads two buffers where the mesh layout has textures, so
        // it takes a set layout of its own rather than another binding in set 0.
        VkDescriptorSetLayout m_splatSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_splatPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_splatPipeline = VK_NULL_HANDLE;
        VkDescriptorPool m_splatDescriptorPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kFramesInFlight> m_splatSets{};
        Buffer m_gaussianBuffer;
        // One per frame in flight: the depth order is rewritten every frame.
        // Device-local now that compute writes it, and also the sort's values-A
        // buffer -- four passes ping-pong and land the result back in here.
        std::array<Buffer, kFramesInFlight> m_gaussianOrder{};
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
            BinCount,
            ScanGroups,
            ScanGroupSums,
            ScanAdd,
            Bin,
            TileRangesReset,
            TileRanges,
            Raster,
            Count
        };

        // Scratch for the sort, one set per frame in flight so that a frame
        // never writes what the previous one is still reading. The per-slot
        // fence wait in renderFrame is what makes that safe without a barrier.
        struct SortBuffers
        {
            Buffer keysA;
            Buffer keysB;
            Buffer valuesB;
            Buffer histogram;
            Buffer digitBase;
            // One projection per splat, written by the cull pass and read
            // by the tile rasteriser.
            Buffer projections;
            // Tile binning: per-splat tile counts, their prefix sum and its
            // scratch, the (tile, splat) pairs the tile sort ping-pongs, and
            // one [first, last) range per tile.
            Buffer tileCounts;
            Buffer tileOffsets;
            Buffer groupSums;
            Buffer pairTileA;
            Buffer pairTileB;
            Buffer pairSplatA;
            Buffer pairSplatB;
            Buffer tileRanges;
        };
        std::array<SortBuffers, kFramesInFlight> m_sortBuffers{};
        VkDescriptorSetLayout m_sortSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_sortPipelineLayout = VK_NULL_HANDLE;
        std::array<VkPipeline, static_cast<std::size_t>(SortStage::Count)>
            m_sortPipelines{};
        VkDescriptorPool m_sortDescriptorPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kFramesInFlight> m_sortSets{};
        // One section per pass, picked with a dynamic offset. Nothing in it
        // changes with the camera, so it is written once at upload.
        Buffer m_sortConstants;
        VkDeviceSize m_sortConstantStride = 0;
        // The tile sort reuses the radix kernels with the pair buffers bound
        // where the depth sort's keys and values usually sit.
        std::array<VkDescriptorSet, kFramesInFlight> m_tileSortSets{};

        // The rasteriser's own target, composited over the mesh image.
        Image m_splatColour;

        std::uint32_t m_pairCapacity = 0;
        std::uint32_t m_pairTileCount = 0;

        std::uint32_t m_sortTileCount = 0;
        // m_gaussianCount rounded up to a whole number of tiles.
        std::uint32_t m_sortPaddedCount = 0;

        Buffer m_vertexBuffer;
        Buffer m_indexBuffer;
        Buffer m_frameUniforms;
        Buffer m_materialUniforms;
        VkDeviceSize m_frameStride = 0;
        VkDeviceSize m_materialStride = 0;

        std::vector<Image> m_textures;
        std::vector<Primitive> m_primitives;
        std::vector<Material> m_materials;

        VkDescriptorPool m_uiPool = VK_NULL_HANDLE;
        bool m_uiReady = false;
        // Per slot: the frame pair followed by a pair per GpuPass. The pass pairs
        // are read only when every scope was bracketed, so a frame with no splats
        // reports nothing for them rather than garbage.
        static constexpr std::uint32_t kFrameQueries = 2;
        static constexpr std::uint32_t kPassQueries =
            2 * static_cast<std::uint32_t>(kGpuPassCount);
        static constexpr std::uint32_t kQueriesPerSlot = kFrameQueries + kPassQueries;
        static constexpr std::uint32_t kAllPassScopes =
            (1u << static_cast<std::uint32_t>(kGpuPassCount)) - 1u;

        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        std::array<bool, kFramesInFlight> m_timestampWritten{};
        // One bit per bracketed GpuPass, cleared when the slot is reset.
        std::array<std::uint32_t, kFramesInFlight> m_passScopeMask{};
        bool m_passScopesEnabled = true;

        RenderSettings m_settings;
        CameraParams m_camera;
        GpuStats m_stats;
        FrameTimer m_frameTimer;
        RollingAverage<FrameTimer::kHistorySize> m_gpuTimeHistory;
        std::array<RollingAverage<FrameTimer::kHistorySize>, kGpuPassCount>
            m_gpuPassHistory{};
        std::function<void()> m_uiCallback;

        UVec2 m_size{0, 0};
        std::uint64_t m_frameCounter = 0;
        double m_fixedTimeStep = 0.0;
        bool m_headless = false;
        bool m_vsync = true;
        bool m_swapchainDirty = false;
        bool m_hasRenderedFrame = false;
    };

} // namespace r4d::vulkan
