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
        Status createDescriptorSets(const Model& model);
        Status createTimestampPool();

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

        Buffer m_vertexBuffer;
        Buffer m_indexBuffer;
        Buffer m_frameUniforms;
        Buffer m_materialUniforms;
        VkDeviceSize m_frameStride = 0;
        VkDeviceSize m_materialStride = 0;

        std::vector<Image> m_textures;
        std::vector<Primitive> m_primitives;
        std::vector<Material> m_materials;

        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        std::array<bool, kFramesInFlight> m_timestampWritten{};

        RenderSettings m_settings;
        CameraParams m_camera;
        GpuStats m_stats;
        FrameTimer m_frameTimer;
        RollingAverage<FrameTimer::kHistorySize> m_gpuTimeHistory;
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
