// VulkanRenderer.cpp
#include "VulkanRenderer.hpp"

#include <radiance4d/core/FileSystem.hpp>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace r4d::vulkan
{
    namespace
    {
        constexpr std::uint32_t kLightCount = 3;

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

        // Must match the clear colour in the D3D12 backend, or the two APIs cannot
        // be compared.
        constexpr float kClearColour[4] = {0.043f, 0.047f, 0.063f, 1.0f};
        constexpr float kClearDepth = 1.0f;

        // Set by the -fvk-*-shift flags the shaders are compiled with.
        constexpr std::uint32_t kTextureBinding = R4D_VK_T_SHIFT;
        constexpr std::uint32_t kSamplerBinding = R4D_VK_S_SHIFT;
        // base colour, metallic-roughness, normal, environment, irradiance, shadow
        constexpr std::uint32_t kTexturesPerMaterial = 6;

        // The splat shader's t0 and t1, moved by the same shift.
        constexpr std::uint32_t kGaussianBinding = R4D_VK_T_SHIFT;
        constexpr std::uint32_t kGaussianOrderBinding = R4D_VK_T_SHIFT + 1;

        // Closer than this the Jacobian is meaningless, so the splat is dropped.
        // Must match the D3D12 backend.
        constexpr float kSplatNearPlane = 0.05f;

        // The sort's b1 and u0..u5, moved by the same shifts.
        constexpr std::uint32_t kSortConstantsBinding = 1;
        constexpr std::uint32_t kSortBufferBinding = R4D_VK_U_SHIFT;
        // t3 and u15 in SplatRaster.cs.hlsl, through the same register shifts.
        constexpr std::uint32_t kSplatDepthBinding = R4D_VK_T_SHIFT + 3;
        constexpr std::uint32_t kSplatTargetBinding = R4D_VK_U_SHIFT + 15;

        // All four must match SplatSort.hlsli, and the D3D12 backend.
        constexpr std::uint32_t kSortGroupSize = 256;
        constexpr std::uint32_t kSortDigitBits = 8;
        constexpr std::uint32_t kSortBuckets = 1u << kSortDigitBits;
        constexpr std::uint32_t kSortPasses = 32 / kSortDigitBits;
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

        // Both halves of the key and value ping-pong, the per-tile histograms
        // and the digit bases.
        constexpr std::uint32_t kSortBufferCount = 15;

        // Must match R4D_TILE_SIZE in SplatTiles.hlsli.
        constexpr std::uint32_t kSplatTileSize = 16;
        // Enough tile ranges for any viewport this renderer will be resized to.
        constexpr std::uint32_t kMaxTiles = ((7680 + kSplatTileSize - 1) / kSplatTileSize) *
                                            ((4320 + kSplatTileSize - 1) / kSplatTileSize);
        constexpr std::uint32_t kPairsPerSplat = 8;
        constexpr VkFormat kSplatColourFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        // Two passes of the radix sort cover a 16-bit tile id.
        constexpr std::uint32_t kTileSortPasses = 2;
        constexpr std::uint32_t kSortConstantSections = kSortPasses + kTileSortPasses;

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


        // Orders every preceding access against every following one. The sort
        // stages each read what the one before wrote, across six buffers, which
        // is exactly what one memory barrier says in one line.
        void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStage,
                           VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                           VkAccessFlags2 dstAccess)
        {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;

            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }

        VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
        {
            return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
        }

        void imageBarrier(VkCommandBuffer commandBuffer, VkImage image,
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                          VkImageAspectFlags aspect, std::uint32_t mipLevels = 1)
        {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = aspect;
            barrier.subresourceRange.levelCount = mipLevels;
            barrier.subresourceRange.layerCount = 1;

            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
    } // namespace

    Result<std::unique_ptr<Renderer>> createRenderer(const RendererDesc& desc)
    {
        return VulkanRenderer::create(desc);
    }

    Result<std::unique_ptr<Renderer>> VulkanRenderer::create(const RendererDesc& desc)
    {
        auto renderer = std::unique_ptr<VulkanRenderer>{new VulkanRenderer{}};
        if (auto status = renderer->initialise(desc); !status)
        {
            return status.error();
        }
        return std::unique_ptr<Renderer>{renderer.release()};
    }

    Status VulkanRenderer::initialise(const RendererDesc& desc)
    {
        m_headless = (desc.windowHandle == nullptr);
        m_vsync = desc.vsync;
        m_passScopesEnabled = desc.passTimings;
        m_fixedTimeStep = desc.fixedTimeStep;
        m_size = desc.initialSize;

        if (m_size.x == 0 || m_size.y == 0)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "RendererDesc::initialSize must be non-zero");
        }

        R4D_TRY(createInstance(m_context, desc.enableDebugLayer, &m_validation));
        R4D_TRY(selectPhysicalDevice(m_context, desc.adapterSelector));

        if (!m_headless)
        {
            R4D_TRY(createSurface(desc.windowHandle));
        }
        R4D_TRY(createLogicalDevice(m_context, m_surface));

        if (m_headless)
        {
            R4D_INFO("Renderer: headless -- no swapchain, rendering to an offscreen target");
            R4D_TRY(createOffscreenTarget(m_size));
        }
        else
        {
            R4D_TRY(createSwapchain(m_size));
        }
        R4D_TRY(createDepthImage(m_size));
        R4D_TRY(createSplatTargets(m_size));
        R4D_TRY(createShadowMap());
        R4D_TRY(createFrameResources());
        R4D_TRY(createDescriptorLayout());
        R4D_TRY(createTimestampPool());

        std::filesystem::path shaderDirectory;
        if (!desc.shaderDirectory.empty())
        {
            shaderDirectory = desc.shaderDirectory;
        }
        else
        {
            const auto beside = executableDirectory() / "shaders";
            std::error_code ec;
            shaderDirectory = std::filesystem::is_directory(beside, ec) ? beside
#ifdef R4D_SHADER_DIR
                                                                       : std::filesystem::path{R4D_SHADER_DIR};
#else
                                                                       : beside;
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

        R4D_INFO("Renderer: initialised, {} frames in flight", kFramesInFlight);
        return Status::Ok();
    }

    Status VulkanRenderer::createSurface(void* windowHandle)
    {
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hinstance = ::GetModuleHandleW(nullptr);
        info.hwnd = static_cast<HWND>(windowHandle);
        R4D_VK(vkCreateWin32SurfaceKHR(m_context.instance, &info, nullptr, &m_surface),
               "vkCreateWin32SurfaceKHR");
        return Status::Ok();
    }

    Status VulkanRenderer::createSwapchain(UVec2 size)
    {
        VkSurfaceCapabilitiesKHR caps{};
        R4D_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_context.physicalDevice, m_surface,
                                                         &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_context.physicalDevice, m_surface,
                                             &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_context.physicalDevice, m_surface,
                                             &formatCount, formats.data());

        VkSurfaceFormatKHR chosen = formats.empty()
                                        ? VkSurfaceFormatKHR{kColourFormat,
                                                             VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                        : formats.front();
        for (const VkSurfaceFormatKHR& format : formats)
        {
            // Match the D3D12 backend, which presents a plain UNORM back buffer and
            // does its own gamma in the shader.
            if (format.format == kColourFormat &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen = format;
                break;
            }
        }
        m_swapchainFormat = chosen.format;

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_context.physicalDevice, m_surface,
                                                  &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_context.physicalDevice, m_surface,
                                                  &presentModeCount, presentModes.data());

        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!m_vsync)
        {
            const auto has = [&](VkPresentModeKHR mode) {
                return std::find(presentModes.begin(), presentModes.end(), mode) !=
                       presentModes.end();
            };
            if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            {
                presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            {
                presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }

        VkExtent2D extent{size.x, size.y};
        if (caps.currentExtent.width != UINT32_MAX)
        {
            extent = caps.currentExtent;
        }
        extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                                   caps.maxImageExtent.height);

        std::uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0)
        {
            imageCount = std::min(imageCount, caps.maxImageCount);
        }

        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = m_surface;
        info.minImageCount = imageCount;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = presentMode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = VK_NULL_HANDLE;

        R4D_VK(vkCreateSwapchainKHR(m_context.device, &info, nullptr, &m_swapchain),
               "vkCreateSwapchainKHR");

        std::uint32_t count = 0;
        vkGetSwapchainImagesKHR(m_context.device, m_swapchain, &count, nullptr);
        m_swapchainImages.resize(count);
        vkGetSwapchainImagesKHR(m_context.device, m_swapchain, &count,
                                m_swapchainImages.data());

        m_swapchainViews.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = m_swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            R4D_VK(vkCreateImageView(m_context.device, &viewInfo, nullptr,
                                     &m_swapchainViews[i]),
                   "vkCreateImageView (swapchain)");
        }

        m_renderFinished.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            R4D_VK(vkCreateSemaphore(m_context.device, &semaphoreInfo, nullptr,
                                     &m_renderFinished[i]),
                   "vkCreateSemaphore (render finished)");
        }

        m_size = UVec2{extent.width, extent.height};
        R4D_INFO("Swapchain: {}x{}, {} images, present mode {}", m_size.x, m_size.y, count,
                 static_cast<int>(presentMode));
        return Status::Ok();
    }

    void VulkanRenderer::destroySwapchain()
    {
        for (VkSemaphore semaphore : m_renderFinished)
        {
            vkDestroySemaphore(m_context.device, semaphore, nullptr);
        }
        m_renderFinished.clear();

        for (VkImageView view : m_swapchainViews)
        {
            vkDestroyImageView(m_context.device, view, nullptr);
        }
        m_swapchainViews.clear();
        m_swapchainImages.clear();
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_context.device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    Status VulkanRenderer::createSplatTargets(UVec2 size)
    {
        destroyImage(m_splatColour);
        auto image = createImage(size.x, size.y, kSplatColourFormat,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        if (!image)
        {
            return image.error();
        }
        m_splatColour = image.value();
        return Status::Ok();
    }

    Status VulkanRenderer::createDepthImage(UVec2 size)
    {
        destroyImage(m_depth);
        // Sampled as well: the tile rasteriser reads it to reject splats
        // behind the mesh.
        auto image = createImage(size.x, size.y, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                                 1, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!image)
        {
            return image.error();
        }
        m_depth = image.value();
        return Status::Ok();
    }

    Status VulkanRenderer::initialiseUi(void* windowHandle)
    {
        // 1.93 allocates separate sampler and sampled-image descriptors as well
        // as combined ones, so all three have to be present.
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 8},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8},
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 16;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
        poolInfo.pPoolSizes = sizes;
        R4D_VK(vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr, &m_uiPool),
               "vkCreateDescriptorPool (ImGui)");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui::GetIO().IniFilename = nullptr;

        if (!ImGui_ImplWin32_Init(windowHandle))
        {
            return makeError(ErrorCode::ApiCallFailed, "ImGui_ImplWin32_Init failed");
        }

        VkFormat colourFormat = m_swapchainFormat;
        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colourFormat;
        rendering.depthAttachmentFormat = kDepthFormat;

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = m_context.instance;
        info.PhysicalDevice = m_context.physicalDevice;
        info.Device = m_context.device;
        info.QueueFamily = m_context.queueFamily;
        info.Queue = m_context.queue;
        info.DescriptorPool = m_uiPool;
        info.MinImageCount = kFramesInFlight;
        info.ImageCount = static_cast<std::uint32_t>(
            std::max<std::size_t>(m_swapchainImages.size(), kFramesInFlight));
        // Dynamic rendering, matching the rest of the backend: no render pass object.
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering;

        if (!ImGui_ImplVulkan_Init(&info))
        {
            return makeError(ErrorCode::ApiCallFailed, "ImGui_ImplVulkan_Init failed");
        }

        m_uiReady = true;
        R4D_INFO("UI: Dear ImGui {} ready", IMGUI_VERSION);
        return Status::Ok();
    }

    void VulkanRenderer::shutdownUi()
    {
        if (!m_uiReady)
        {
            return;
        }
        m_uiReady = false;
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (m_uiPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_context.device, m_uiPool, nullptr);
            m_uiPool = VK_NULL_HANDLE;
        }
    }

    void VulkanRenderer::renderUi(VkCommandBuffer commandBuffer)
    {
        if (!m_uiReady)
        {
            return;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (m_uiCallback)
        {
            m_uiCallback();
        }

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    Status VulkanRenderer::createShadowMap()
    {
        destroyImage(m_shadowMap);
        auto image = createImage(kShadowMapSize, kShadowMapSize, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                                 1, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!image)
        {
            return image.error();
        }
        m_shadowMap = image.value();
        m_shadowMapInitialised = false;
        return Status::Ok();
    }

    Mat4 VulkanRenderer::lightViewProjection() const
    {
        // Must match the D3D12 backend exactly, or the two diverge.
        const Vec3 direction = glm::normalize(Vec3{-0.4f, 0.8f, -0.45f});
        const Vec3 centre = m_sceneBounds.valid() ? m_sceneBounds.centre() : Vec3{0.0f};
        const float radius =
            m_sceneBounds.valid() ? std::max(m_sceneBounds.radius(), 0.01f) : 1.0f;

        const Vec3 eye = centre + direction * (radius * 2.0f);
        const Vec3 up = std::abs(direction.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f}
                                                      : Vec3{0.0f, 1.0f, 0.0f};
        const Mat4 view = glm::lookAt(eye, centre, up);
        const Mat4 projection =
            glm::ortho(-radius, radius, -radius, radius, 0.05f, radius * 4.5f);
        return projection * view;
    }

    void VulkanRenderer::renderShadowPass(VkCommandBuffer commandBuffer)
    {
        if (m_primitives.empty() || m_shadowPipeline == VK_NULL_HANDLE ||
            m_shadowMap.handle == VK_NULL_HANDLE)
        {
            return;
        }

        imageBarrier(commandBuffer, m_shadowMap.handle,
                     m_shadowMapInitialised ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
        m_shadowMapInitialised = true;

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = m_shadowMap.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil.depth = kClearDepth;

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {kShadowMapSize, kShadowMapSize};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(commandBuffer, &rendering);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                            static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer.handle, &vertexOffset);
        vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

        const std::uint32_t frameIndex =
            static_cast<std::uint32_t>(m_frameCounter % kFramesInFlight);
        for (const Primitive& primitive : m_primitives)
        {
            const std::uint32_t materialIndex =
                primitive.materialIndex < m_materialSets.size() ? primitive.materialIndex : 0;
            const std::uint32_t offsets[2] = {
                static_cast<std::uint32_t>(frameIndex * m_frameStride),
                static_cast<std::uint32_t>(materialIndex * m_materialStride)};
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &m_materialSets[materialIndex], 2,
                                    offsets);
            vkCmdDrawIndexed(commandBuffer, primitive.indexCount, 1, primitive.firstIndex,
                             static_cast<std::int32_t>(primitive.vertexOffset), 0);
        }

        vkCmdEndRendering(commandBuffer);

        imageBarrier(commandBuffer, m_shadowMap.handle,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    Status VulkanRenderer::createShadowPipeline(const std::filesystem::path& shaderDirectory)
    {
        auto vertexModule = loadShaderModule(shaderDirectory, "Shadow.vs.spv");
        if (!vertexModule)
        {
            return vertexModule.error();
        }
        auto fragmentModule = loadShaderModule(shaderDirectory, "Shadow.ps.spv");
        if (!fragmentModule)
        {
            vkDestroyShaderModule(m_context.device, vertexModule.value(), nullptr);
            return fragmentModule.error();
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule.value();
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule.value();
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        // Only position: the shadow shader consumes nothing else, and declaring
        // unused attributes makes the validation layer complain.
        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(std::size(attributes));
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;
        // Same bias as the D3D12 shadow pipeline.
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 120.0f;
        raster.depthBiasSlopeFactor = 2.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
        dynamic.pDynamicStates = dynamicStates;

        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.depthAttachmentFormat = kDepthFormat;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = m_pipelineLayout;

        const VkResult result = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1,
                                                          &info, nullptr, &m_shadowPipeline);
        vkDestroyShaderModule(m_context.device, vertexModule.value(), nullptr);
        vkDestroyShaderModule(m_context.device, fragmentModule.value(), nullptr);
        if (result != VK_SUCCESS)
        {
            return vulkanError(ErrorCode::ApiCallFailed, result,
                               "vkCreateGraphicsPipelines (shadow)");
        }
        return Status::Ok();
    }

    Status VulkanRenderer::createOffscreenTarget(UVec2 size)
    {
        destroyImage(m_offscreen);
        auto image = createImage(size.x, size.y, kColourFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 1, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!image)
        {
            return image.error();
        }
        m_offscreen = image.value();
        m_size = size;
        R4D_INFO("Offscreen target: {}x{}", size.x, size.y);
        return Status::Ok();
    }

    Status VulkanRenderer::createFrameResources()
    {
        for (Frame& frame : m_frames)
        {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = m_context.queueFamily;
            R4D_VK(vkCreateCommandPool(m_context.device, &poolInfo, nullptr,
                                       &frame.commandPool),
                   "vkCreateCommandPool");

            VkCommandBufferAllocateInfo allocInfo{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            R4D_VK(vkAllocateCommandBuffers(m_context.device, &allocInfo, &frame.commandBuffer),
                   "vkAllocateCommandBuffers");

            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            R4D_VK(vkCreateSemaphore(m_context.device, &semaphoreInfo, nullptr, &frame.acquired),
                   "vkCreateSemaphore (acquired)");

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            R4D_VK(vkCreateFence(m_context.device, &fenceInfo, nullptr, &frame.inFlight),
                   "vkCreateFence");
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_context.queueFamily;
        R4D_VK(vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_uploadPool),
               "vkCreateCommandPool (upload)");

        const VkDeviceSize alignment =
            m_context.properties.limits.minUniformBufferOffsetAlignment;
        m_frameStride = alignUp(sizeof(FrameConstants), alignment);
        m_materialStride = alignUp(sizeof(MaterialConstants), alignment);

        auto frameBuffer = createBuffer(m_frameStride * kFramesInFlight,
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        "frame uniforms");
        if (!frameBuffer)
        {
            return frameBuffer.error();
        }
        m_frameUniforms = frameBuffer.value();

        return Status::Ok();
    }

    Status VulkanRenderer::createDescriptorLayout()
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable =
            m_context.properties.limits.maxSamplerAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy =
            std::min(8.0f, m_context.properties.limits.maxSamplerAnisotropy);
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        R4D_VK(vkCreateSampler(m_context.device, &samplerInfo, nullptr, &m_sampler),
               "vkCreateSampler");

        // Plain linear, clamped in v: see the note in the shader.
        VkSamplerCreateInfo environmentInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        environmentInfo.magFilter = VK_FILTER_LINEAR;
        environmentInfo.minFilter = VK_FILTER_LINEAR;
        environmentInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        environmentInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        environmentInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        environmentInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        environmentInfo.anisotropyEnable = VK_FALSE;
        environmentInfo.maxLod = VK_LOD_CLAMP_NONE;
        R4D_VK(vkCreateSampler(m_context.device, &environmentInfo, nullptr,
                               &m_environmentSampler),
               "vkCreateSampler (environment)");

        VkSamplerCreateInfo shadowInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        shadowInfo.magFilter = VK_FILTER_LINEAR;
        shadowInfo.minFilter = VK_FILTER_LINEAR;
        shadowInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        shadowInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        shadowInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        shadowInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        // White border: outside the map is lit, not shadowed.
        shadowInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        shadowInfo.compareEnable = VK_TRUE;
        shadowInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        R4D_VK(vkCreateSampler(m_context.device, &shadowInfo, nullptr, &m_shadowSampler),
               "vkCreateSampler (shadow)");

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        // Dynamic so one set per material can serve every frame slot.
        bindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr});
        bindings.push_back({1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        for (std::uint32_t i = 0; i < kTexturesPerMaterial; ++i)
        {
            bindings.push_back({kTextureBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        }
        bindings.push_back({kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        bindings.push_back({kSamplerBinding + 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        bindings.push_back({kSamplerBinding + 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        R4D_VK(vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, nullptr,
                                           &m_setLayout),
               "vkCreateDescriptorSetLayout");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_setLayout;
        R4D_VK(vkCreatePipelineLayout(m_context.device, &pipelineLayoutInfo, nullptr,
                                      &m_pipelineLayout),
               "vkCreatePipelineLayout");

        return Status::Ok();
    }

    Status VulkanRenderer::createTimestampPool()
    {
        if (m_context.timestampPeriod <= 0.0f)
        {
            R4D_WARN("Profiler: device reports no timestamp period; GPU timings disabled");
            return Status::Ok();
        }
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kFramesInFlight * kQueriesPerSlot;
        R4D_VK(vkCreateQueryPool(m_context.device, &info, nullptr, &m_timestampPool),
               "vkCreateQueryPool");
        R4D_INFO("Profiler: timestamp queries ready, period {} ns, {} per frame slot "
                 "({} pass scopes)",
                 m_context.timestampPeriod, kQueriesPerSlot, kGpuPassCount);
        return Status::Ok();
    }

    void VulkanRenderer::beginPassScope(VkCommandBuffer commandBuffer,
                                        std::uint32_t frameIndex, GpuPass pass)
    {
        if (m_timestampPool == VK_NULL_HANDLE || !m_passScopesEnabled ||
            pass == GpuPass::Count)
        {
            return;
        }
        // ALL_COMMANDS so the timestamp lands once everything issued before it
        // has finished, which is what EndQuery gives on the other backend.
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             m_timestampPool, passQueryIndex(frameIndex, pass));
    }

    void VulkanRenderer::endPassScope(VkCommandBuffer commandBuffer,
                                      std::uint32_t frameIndex, GpuPass pass)
    {
        if (m_timestampPool == VK_NULL_HANDLE || !m_passScopesEnabled ||
            pass == GpuPass::Count)
        {
            return;
        }
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             m_timestampPool, passQueryIndex(frameIndex, pass) + 1);
        m_passScopeMask[frameIndex] |= 1u << static_cast<std::uint32_t>(pass);
    }

    Result<VkShaderModule> VulkanRenderer::loadShaderModule(
        const std::filesystem::path& directory, std::string_view name) const
    {
        const auto path = directory / name;
        auto blob = readBinaryFile(path);
        if (!blob)
        {
            return makeError(ErrorCode::ShaderLoadFailed,
                             std::format("cannot read '{}': {}", path.string(),
                                         blob.error().message));
        }
        if (blob.value().empty() || blob.value().size() % 4 != 0)
        {
            return makeError(ErrorCode::ShaderLoadFailed,
                             std::format("'{}' is not valid SPIR-V", path.string()));
        }

        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = blob.value().size();
        info.pCode = reinterpret_cast<const std::uint32_t*>(blob.value().data());

        VkShaderModule module = VK_NULL_HANDLE;
        R4D_VK(vkCreateShaderModule(m_context.device, &info, nullptr, &module),
               "vkCreateShaderModule");
        return module;
    }

    Status VulkanRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
    {
        auto vertexModule = loadShaderModule(shaderDirectory, "Pbr.vs.spv");
        if (!vertexModule)
        {
            return vertexModule.error();
        }
        auto fragmentModule = loadShaderModule(shaderDirectory, "Pbr.ps.spv");
        if (!fragmentModule)
        {
            vkDestroyShaderModule(m_context.device, vertexModule.value(), nullptr);
            return fragmentModule.error();
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule.value();
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule.value();
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(std::size(attributes));
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_BACK_BIT;
        // The shaders are compiled with -fvk-invert-y, which puts clip space back
        // into the D3D convention, so the D3D12 winding carries over unchanged.
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
        dynamic.pDynamicStates = dynamicStates;

        const VkFormat colourFormat = m_headless ? kColourFormat : m_swapchainFormat;
        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colourFormat;
        rendering.depthAttachmentFormat = kDepthFormat;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;   // dynamic rendering: no render pass object
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = m_pipelineLayout;
        info.renderPass = VK_NULL_HANDLE;

        const VkResult result = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1,
                                                          &info, nullptr, &m_pipeline);
        vkDestroyShaderModule(m_context.device, vertexModule.value(), nullptr);
        vkDestroyShaderModule(m_context.device, fragmentModule.value(), nullptr);
        if (result != VK_SUCCESS)
        {
            return vulkanError(ErrorCode::ApiCallFailed, result, "vkCreateGraphicsPipelines");
        }

        return Status::Ok();
    }

    Status VulkanRenderer::createSplatPipeline(const std::filesystem::path& shaderDirectory)
    {
        // One texture, fetched by its coordinate, so there is no sampler and
        // nothing else to bind: the tile rasteriser already did the work.
        const VkDescriptorSetLayoutBinding bindings[] = {
            {kGaussianBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(std::size(bindings));
        layoutInfo.pBindings = bindings;
        R4D_VK(vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, nullptr,
                                           &m_splatSetLayout),
               "vkCreateDescriptorSetLayout (splat)");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_splatSetLayout;
        R4D_VK(vkCreatePipelineLayout(m_context.device, &pipelineLayoutInfo, nullptr,
                                      &m_splatPipelineLayout),
               "vkCreatePipelineLayout (splat)");

        auto vertexModule = loadShaderModule(shaderDirectory, "SplatComposite.vs.spv");
        if (!vertexModule)
        {
            return vertexModule.error();
        }
        auto fragmentModule = loadShaderModule(shaderDirectory, "SplatComposite.ps.spv");
        if (!fragmentModule)
        {
            vkDestroyShaderModule(m_context.device, vertexModule.value(), nullptr);
            return fragmentModule.error();
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule.value();
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule.value();
        stages[1].pName = "main";

        // The quad comes from the vertex index, so there is nothing to fetch.
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // The quad's winding is not fixed, so nothing may be culled.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // The depth test happened per splat in the rasteriser.
        // themselves by the draw order instead.
        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        // Premultiplied alpha, which is what the pixel shader emits.
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
        dynamic.pDynamicStates = dynamicStates;

        const VkFormat colourFormat = m_headless ? kColourFormat : m_swapchainFormat;
        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colourFormat;
        rendering.depthAttachmentFormat = kDepthFormat;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = m_splatPipelineLayout;

        const VkResult result = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1,
                                                          &info, nullptr, &m_splatPipeline);
        vkDestroyShaderModule(m_context.device, vertexModule.value(), nullptr);
        vkDestroyShaderModule(m_context.device, fragmentModule.value(), nullptr);
        if (result != VK_SUCCESS)
        {
            return vulkanError(ErrorCode::ApiCallFailed, result,
                               "vkCreateGraphicsPipelines (splat)");
        }
        return Status::Ok();
    }

    Result<VulkanRenderer::Buffer> VulkanRenderer::createBuffer(
        VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        const char* name)
    {
        Buffer buffer;
        buffer.size = size;

        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        R4D_VK(vkCreateBuffer(m_context.device, &info, nullptr, &buffer.handle),
               std::format("vkCreateBuffer ({})", name));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_context.device, buffer.handle, &requirements);

        const std::uint32_t typeIndex =
            findMemoryType(m_context, requirements.memoryTypeBits, properties);
        if (typeIndex == UINT32_MAX)
        {
            return makeError(ErrorCode::ApiCallFailed,
                             std::format("no memory type for {}", name));
        }

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = typeIndex;
        R4D_VK(vkAllocateMemory(m_context.device, &allocInfo, nullptr, &buffer.memory),
               std::format("vkAllocateMemory ({})", name));
        R4D_VK(vkBindBufferMemory(m_context.device, buffer.handle, buffer.memory, 0),
               std::format("vkBindBufferMemory ({})", name));

        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            R4D_VK(vkMapMemory(m_context.device, buffer.memory, 0, size, 0, &buffer.mapped),
                   std::format("vkMapMemory ({})", name));
        }
        return buffer;
    }

    void VulkanRenderer::destroyBuffer(Buffer& buffer)
    {
        if (buffer.mapped != nullptr)
        {
            vkUnmapMemory(m_context.device, buffer.memory);
            buffer.mapped = nullptr;
        }
        if (buffer.handle != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_context.device, buffer.handle, nullptr);
            buffer.handle = VK_NULL_HANDLE;
        }
        if (buffer.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_context.device, buffer.memory, nullptr);
            buffer.memory = VK_NULL_HANDLE;
        }
        buffer.size = 0;
    }

    Result<VulkanRenderer::Image> VulkanRenderer::createImage(
        std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
        std::uint32_t mipLevels, VkImageAspectFlags aspect)
    {
        Image image;
        image.format = format;
        image.mipLevels = mipLevels;

        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = mipLevels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        R4D_VK(vkCreateImage(m_context.device, &info, nullptr, &image.handle), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_context.device, image.handle, &requirements);
        const std::uint32_t typeIndex = findMemoryType(m_context, requirements.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (typeIndex == UINT32_MAX)
        {
            return makeError(ErrorCode::ApiCallFailed, "no device-local memory type for image");
        }

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = typeIndex;
        R4D_VK(vkAllocateMemory(m_context.device, &allocInfo, nullptr, &image.memory),
               "vkAllocateMemory (image)");
        R4D_VK(vkBindImageMemory(m_context.device, image.handle, image.memory, 0),
               "vkBindImageMemory");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.handle;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = 1;
        R4D_VK(vkCreateImageView(m_context.device, &viewInfo, nullptr, &image.view),
               "vkCreateImageView");

        return image;
    }

    void VulkanRenderer::destroyImage(Image& image)
    {
        if (image.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_context.device, image.view, nullptr);
            image.view = VK_NULL_HANDLE;
        }
        if (image.handle != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_context.device, image.handle, nullptr);
            image.handle = VK_NULL_HANDLE;
        }
        if (image.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_context.device, image.memory, nullptr);
            image.memory = VK_NULL_HANDLE;
        }
    }

    Result<VkCommandBuffer> VulkanRenderer::beginOneShot(VkCommandPool pool)
    {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        R4D_VK(vkAllocateCommandBuffers(m_context.device, &allocInfo, &commandBuffer),
               "vkAllocateCommandBuffers (one shot)");

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        R4D_VK(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");
        return commandBuffer;
    }

    Status VulkanRenderer::endOneShot(VkCommandPool pool, VkCommandBuffer commandBuffer)
    {
        R4D_VK(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = commandBuffer;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commandInfo;

        R4D_VK(vkQueueSubmit2(m_context.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        R4D_VK(vkQueueWaitIdle(m_context.queue), "vkQueueWaitIdle");

        vkFreeCommandBuffers(m_context.device, pool, 1, &commandBuffer);
        return Status::Ok();
    }

    Result<VulkanRenderer::Image> VulkanRenderer::createTexture(
        const ImageRgba8& source, bool srgb, VkCommandBuffer commandBuffer,
        std::vector<Buffer>& staging)
    {
        const std::vector<ImageRgba8> levels = generateMipChain(source);
        const auto mipLevels = static_cast<std::uint32_t>(levels.size());
        const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

        auto image = createImage(source.width, source.height, format,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 mipLevels, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!image)
        {
            return image.error();
        }

        VkDeviceSize totalBytes = 0;
        for (const ImageRgba8& level : levels)
        {
            totalBytes += level.rgba.size();
        }

        auto upload = createBuffer(totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   "texture upload");
        if (!upload)
        {
            return upload.error();
        }

        std::vector<VkBufferImageCopy> copies;
        VkDeviceSize offset = 0;
        for (std::uint32_t level = 0; level < mipLevels; ++level)
        {
            std::memcpy(static_cast<std::byte*>(upload.value().mapped) + offset,
                        levels[level].rgba.data(), levels[level].rgba.size());

            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = level;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {levels[level].width, levels[level].height, 1};
            copies.push_back(copy);

            offset += levels[level].rgba.size();
        }

        imageBarrier(commandBuffer, image.value().handle, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

        vkCmdCopyBufferToImage(commandBuffer, upload.value().handle, image.value().handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());

        imageBarrier(commandBuffer, image.value().handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

        staging.push_back(upload.value());
        return image.value();
    }

    void VulkanRenderer::destroySplatBuffers()
    {
        if (m_splatDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_context.device, m_splatDescriptorPool, nullptr);
            m_splatDescriptorPool = VK_NULL_HANDLE;
            m_splatSets = {};
        }
        if (m_sortDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_context.device, m_sortDescriptorPool, nullptr);
            m_sortDescriptorPool = VK_NULL_HANDLE;
            m_sortSets = {};
        }
        destroyBuffer(m_gaussianBuffer);
        destroyBuffer(m_sortConstants);
        for (Buffer& order : m_gaussianOrder)
        {
            destroyBuffer(order);
        }
        for (SortBuffers& sort : m_sortBuffers)
        {
            destroyBuffer(sort.keysA);
            destroyBuffer(sort.keysB);
            destroyBuffer(sort.valuesB);
            destroyBuffer(sort.histogram);
            destroyBuffer(sort.digitBase);
            destroyBuffer(sort.tileCounts);
            destroyBuffer(sort.tileOffsets);
            destroyBuffer(sort.groupSums);
            destroyBuffer(sort.pairTileA);
            destroyBuffer(sort.pairTileB);
            destroyBuffer(sort.pairSplatA);
            destroyBuffer(sort.pairSplatB);
            destroyBuffer(sort.tileRanges);
            destroyBuffer(sort.projections);
        }
    }

    Status VulkanRenderer::uploadGaussians(const GaussianCloud& cloud)
    {
        waitForGpuIdle();
        destroySplatBuffers();
        m_gaussianCount = 0;
        m_sortTileCount = 0;
        m_sortPaddedCount = 0;

        if (cloud.empty())
        {
            return Status::Ok();
        }

        const auto count = static_cast<std::uint32_t>(cloud.count());
        const VkDeviceSize splatBytes = static_cast<VkDeviceSize>(count) * sizeof(Gaussian);

        auto gaussianBuffer = createBuffer(splatBytes,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                           "gaussian buffer");
        if (!gaussianBuffer)
        {
            return gaussianBuffer.error();
        }
        m_gaussianBuffer = gaussianBuffer.value();

        auto staging = createBuffer(splatBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    "gaussian staging");
        if (!staging)
        {
            return staging.error();
        }
        std::memcpy(staging.value().mapped, cloud.gaussians.data(),
                    static_cast<std::size_t>(splatBytes));

        auto commandBuffer = beginOneShot(m_uploadPool);
        if (!commandBuffer)
        {
            return commandBuffer.error();
        }

        VkBufferCopy copy{0, 0, splatBytes};
        vkCmdCopyBuffer(commandBuffer.value(), staging.value().handle, m_gaussianBuffer.handle,
                        1, &copy);

        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        // The sort reads the positions out of the same buffer the vertex
        // shader rasterises from, so both stages are in the sync scope.
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = m_gaussianBuffer.handle;
        barrier.size = VK_WHOLE_SIZE;

        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer.value(), &dependency);

        R4D_TRY(endOneShot(m_uploadPool, commandBuffer.value()));
        destroyBuffer(staging.value());

        // Every kernel but the key build runs without a bounds check, so the
        // sort buffers hold a whole number of tiles and the key build fills the
        // tail with the largest possible key.
        m_sortTileCount = (count + kSortGroupSize - 1) / kSortGroupSize;
        m_sortPaddedCount = m_sortTileCount * kSortGroupSize;

        m_pairCapacity =
            ((count * kPairsPerSplat + kSortGroupSize - 1) / kSortGroupSize) * kSortGroupSize;
        m_pairTileCount = m_pairCapacity / kSortGroupSize;

        // Digit-major, one row of per-tile counts per digit. The tile sort runs
        // over the pairs, which is the larger of the two.
        const VkDeviceSize histogramEntries =
            static_cast<VkDeviceSize>(kSortBuckets) *
            std::max(m_sortTileCount, m_pairTileCount);

        const auto createSortBuffer = [&](VkDeviceSize entries, const char* name,
                                          Buffer& out) -> Status {
            auto buffer = createBuffer(entries * sizeof(std::uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, name);
            if (!buffer)
            {
                return buffer.error();
            }
            out = buffer.value();
            return Status::Ok();
        };

        for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            SortBuffers& sort = m_sortBuffers[i];
            // The order the vertex shader reads is the sort's values-A buffer:
            // with an even number of passes the result lands back in it.
            R4D_TRY(createSortBuffer(m_sortPaddedCount, "gaussian order",
                                     m_gaussianOrder[i]));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, "splat sort keys A", sort.keysA));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, "splat sort keys B", sort.keysB));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, "splat sort values B",
                                     sort.valuesB));
            R4D_TRY(createSortBuffer(histogramEntries, "splat sort histogram",
                                     sort.histogram));
            R4D_TRY(createSortBuffer(kSortBuckets, "splat sort digit bases",
                                     sort.digitBase));

            R4D_TRY(createSortBuffer(m_sortPaddedCount, "splat tile counts",
                                     sort.tileCounts));
            R4D_TRY(createSortBuffer(m_sortPaddedCount, "splat tile offsets",
                                     sort.tileOffsets));
            R4D_TRY(createSortBuffer(m_sortTileCount + 1, "splat group sums",
                                     sort.groupSums));
            R4D_TRY(createSortBuffer(m_pairCapacity, "splat pair tiles A",
                                     sort.pairTileA));
            R4D_TRY(createSortBuffer(m_pairCapacity, "splat pair tiles B",
                                     sort.pairTileB));
            R4D_TRY(createSortBuffer(m_pairCapacity, "splat pair splats A",
                                     sort.pairSplatA));
            R4D_TRY(createSortBuffer(m_pairCapacity, "splat pair splats B",
                                     sort.pairSplatB));
            R4D_TRY(createSortBuffer(kMaxTiles * 2, "splat tile ranges",
                                     sort.tileRanges));

            // One projection per real splat; the sort's padding never reaches
            // the draw, so it needs none.
            auto projections = createBuffer(
                static_cast<VkDeviceSize>(count) * sizeof(SplatProjection),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                "splat projections");
            if (!projections)
            {
                return projections.error();
            }
            sort.projections = projections.value();

        }

        // A pass's constants never change once the count is known, so the whole
        // ping-pong schedule is written here and picked out by dynamic offset.
        m_sortConstantStride =
            alignUp(sizeof(SortConstants),
                    m_context.properties.limits.minUniformBufferOffsetAlignment);
        auto sortConstants = createBuffer(m_sortConstantStride * kSortConstantSections,
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          "splat sort constants");
        if (!sortConstants)
        {
            return sortConstants.error();
        }
        m_sortConstants = sortConstants.value();

        for (std::uint32_t section = 0; section < kSortConstantSections; ++section)
        {
            const bool tilePass = section >= kSortPasses;
            const std::uint32_t pass = tilePass ? section - kSortPasses : section;

            SortConstants constants{};
            constants.splatCount = count;
            constants.pairCapacity = m_pairCapacity;
            // The tile sort runs the same kernels over the pair buffers, so its
            // sections differ only in how many elements there are.
            constants.count = tilePass ? m_pairCapacity : m_sortPaddedCount;
            constants.tileCount = tilePass ? m_pairTileCount : m_sortTileCount;
            // The binning scan leaves the pair total one past the per-group
            // sums; a pair pass sizes itself from it instead of the capacity.
            constants.pairTotalSlot = m_sortTileCount;
            constants.pairPass = tilePass ? 1u : 0u;
            constants.shift = pass * kSortDigitBits;
            // Pass 0 reads A, and an even pass count brings the result home.
            constants.sourceIsB = pass & 1u;
            std::memcpy(static_cast<std::byte*>(m_sortConstants.mapped) +
                            section * m_sortConstantStride,
                        &constants, sizeof(constants));
        }

        m_gaussianCount = count;

        R4D_TRY(createSplatDescriptors());
        R4D_TRY(createSortDescriptors());

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
    VkDeviceSize VulkanRenderer::sortBufferBytes() const
    {
        if (m_gaussianCount == 0)
        {
            return 0;
        }
        // Per frame slot: the order, both halves of the key ping-pong and the
        // spare value half, the per-splat tile counts and their prefix sum, the
        // scan's group sums, the digit bases and the per-tile ranges.
        const VkDeviceSize entries = static_cast<VkDeviceSize>(6) * m_sortPaddedCount +
                                     m_sortTileCount + 1 + kSortBuckets + kMaxTiles * 2;
        return (entries * sizeof(std::uint32_t) +
                static_cast<VkDeviceSize>(m_gaussianCount) * sizeof(SplatProjection)) *
               kFramesInFlight;
    }

    // The four pair buffers and the histogram the tile sort shares with the
    // depth sort, which between them are most of what the splat path costs.
    VkDeviceSize VulkanRenderer::pairBufferBytes() const
    {
        if (m_gaussianCount == 0)
        {
            return 0;
        }
        const VkDeviceSize entries = static_cast<VkDeviceSize>(4) * m_pairCapacity +
                                     static_cast<VkDeviceSize>(kSortBuckets) *
                                         std::max(m_sortTileCount, m_pairTileCount);
        return entries * sizeof(std::uint32_t) * kFramesInFlight;
    }

    Status VulkanRenderer::createSortPipelines(const std::filesystem::path& shaderDirectory)
    {
        // Binding numbers come from the register shifts the shaders are built
        // with, the same way the splat pass gets its t0 and t1.
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        bindings.push_back({kSortConstantsBinding,
                            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        bindings.push_back({kGaussianBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        for (std::uint32_t i = 0; i < kSortBufferCount; ++i)
        {
            bindings.push_back({kSortBufferBinding + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        // Only the rasteriser uses these, but one layout across every splat
        // kernel keeps the binding code in one place.
        bindings.push_back({kSplatDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        bindings.push_back({kSplatTargetBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        R4D_VK(vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, nullptr,
                                           &m_sortSetLayout),
               "vkCreateDescriptorSetLayout (splat sort)");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_sortSetLayout;
        R4D_VK(vkCreatePipelineLayout(m_context.device, &pipelineLayoutInfo, nullptr,
                                      &m_sortPipelineLayout),
               "vkCreatePipelineLayout (splat sort)");

        // Indexed by SortStage, so the order of these must match the enum.
        static constexpr std::string_view kStageShaders[] = {
            "SplatCull.cs.spv",          "SplatSortHistogram.cs.spv",
            "SplatSortScanRows.cs.spv",  "SplatSortScanTotals.cs.spv",
            "SplatSortScatter.cs.spv",   "SplatBinCount.cs.spv",
            "SplatScanGroups.cs.spv",    "SplatScanGroupSums.cs.spv",
            "SplatScanAdd.cs.spv",       "SplatBin.cs.spv",
            "SplatTileRangesReset.cs.spv", "SplatTileRanges.cs.spv",
            "SplatRaster.cs.spv"};
        static_assert(std::size(kStageShaders) ==
                      static_cast<std::size_t>(SortStage::Count));

        for (std::size_t i = 0; i < std::size(kStageShaders); ++i)
        {
            auto shaderModule = loadShaderModule(shaderDirectory, kStageShaders[i]);
            if (!shaderModule)
            {
                return shaderModule.error();
            }

            VkComputePipelineCreateInfo info{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = shaderModule.value();
            info.stage.pName = "main";
            info.layout = m_sortPipelineLayout;

            const VkResult result =
                vkCreateComputePipelines(m_context.device, VK_NULL_HANDLE, 1, &info,
                                         nullptr, &m_sortPipelines[i]);
            vkDestroyShaderModule(m_context.device, shaderModule.value(), nullptr);
            R4D_VK(result, "vkCreateComputePipelines (splat sort)");
        }

        return Status::Ok();
    }

    Status VulkanRenderer::createSortDescriptors()
    {
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFramesInFlight * 4},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             kFramesInFlight * 2 * (kSortBufferCount + 1)},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight * 2},
        };

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = kFramesInFlight * 2;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
        poolInfo.pPoolSizes = sizes;
        R4D_VK(vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr,
                                      &m_sortDescriptorPool),
               "vkCreateDescriptorPool (splat sort)");

        const std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{m_sortSetLayout,
                                                                         m_sortSetLayout};
        VkDescriptorSetAllocateInfo allocInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_sortDescriptorPool;
        allocInfo.descriptorSetCount = kFramesInFlight;
        allocInfo.pSetLayouts = layouts.data();
        R4D_VK(vkAllocateDescriptorSets(m_context.device, &allocInfo, m_sortSets.data()),
               "vkAllocateDescriptorSets (splat sort)");
        R4D_VK(vkAllocateDescriptorSets(m_context.device, &allocInfo,
                                        m_tileSortSets.data()),
               "vkAllocateDescriptorSets (splat tile sort)");

        for (std::uint32_t pass = 0; pass < 2; ++pass)
        for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const SortBuffers& sort = m_sortBuffers[i];
            const VkDescriptorSet set = (pass == 0) ? m_sortSets[i] : m_tileSortSets[i];

            // Must match the u0..u14 order in SplatSort.hlsli and SplatTiles.hlsli.
            // The tile sort is the same kernels over the pairs, so its set puts
            // the pair buffers where the depth keys and values usually sit.
            const Buffer* const sortBuffers[kSortBufferCount] = {
                (pass == 0) ? &sort.keysA : &sort.pairTileA,
                (pass == 0) ? &sort.keysB : &sort.pairTileB,
                (pass == 0) ? &m_gaussianOrder[i] : &sort.pairSplatA,
                (pass == 0) ? &sort.valuesB : &sort.pairSplatB,
                &sort.histogram,   &sort.digitBase,   &sort.projections,
                &sort.tileCounts,  &sort.tileOffsets, &sort.groupSums,
                &sort.pairTileA,   &sort.pairTileB,
                &sort.pairSplatA,  &sort.pairSplatB,  &sort.tileRanges};

            VkDescriptorBufferInfo frameInfo{m_frameUniforms.handle, 0,
                                             sizeof(FrameConstants)};
            VkDescriptorBufferInfo constantsInfo{m_sortConstants.handle, 0,
                                                 sizeof(SortConstants)};
            VkDescriptorBufferInfo gaussianInfo{m_gaussianBuffer.handle, 0, VK_WHOLE_SIZE};
            std::array<VkDescriptorBufferInfo, kSortBufferCount> bufferInfos{};

            std::vector<VkWriteDescriptorSet> writes;
            const auto write = [&](std::uint32_t binding, VkDescriptorType type,
                                   const VkDescriptorBufferInfo* buffer) {
                VkWriteDescriptorSet entry{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                entry.dstSet = set;
                entry.dstBinding = binding;
                entry.descriptorCount = 1;
                entry.descriptorType = type;
                entry.pBufferInfo = buffer;
                writes.push_back(entry);
            };

            write(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &frameInfo);
            write(kSortConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                  &constantsInfo);
            write(kGaussianBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &gaussianInfo);
            for (std::uint32_t b = 0; b < kSortBufferCount; ++b)
            {
                bufferInfos[b] = {sortBuffers[b]->handle, 0, VK_WHOLE_SIZE};
                write(kSortBufferBinding + b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      &bufferInfos[b]);
            }

            // The rasteriser's two images. They outlive an upload but are
            // recreated on resize, which rewrites these.
            VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, m_depth.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo targetInfo{VK_NULL_HANDLE, m_splatColour.view,
                                             VK_IMAGE_LAYOUT_GENERAL};
            const auto writeImage = [&](std::uint32_t binding, VkDescriptorType type,
                                        const VkDescriptorImageInfo* image) {
                VkWriteDescriptorSet entry{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                entry.dstSet = set;
                entry.dstBinding = binding;
                entry.descriptorCount = 1;
                entry.descriptorType = type;
                entry.pImageInfo = image;
                writes.push_back(entry);
            };
            writeImage(kSplatDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo);
            writeImage(kSplatTargetBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &targetInfo);

            vkUpdateDescriptorSets(m_context.device,
                                   static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }

        return Status::Ok();
    }

    void VulkanRenderer::sortSplats(VkCommandBuffer commandBuffer,
                                    std::uint32_t frameIndex)
    {
        if (m_gaussianCount == 0 || m_sortPipelineLayout == VK_NULL_HANDLE)
        {
            return;
        }

        // This slot's buffers were last touched two frames ago and renderFrame
        // has already waited on that frame's fence, so nothing here needs a
        // barrier against the previous frame's reads.
        const auto dispatch = [&](SortStage stage, std::uint32_t section,
                                  std::uint32_t groups) {
            // Sections past the depth passes belong to the tile sort, which
            // reads the pair buffers through its own set.
            const bool tilePass = section >= kSortPasses;
            const std::uint32_t offsets[] = {
                static_cast<std::uint32_t>(frameIndex * m_frameStride),
                static_cast<std::uint32_t>(section * m_sortConstantStride)};
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              m_sortPipelines[static_cast<std::size_t>(stage)]);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_sortPipelineLayout, 0, 1,
                                    tilePass ? &m_tileSortSets[frameIndex]
                                             : &m_sortSets[frameIndex],
                                    static_cast<std::uint32_t>(std::size(offsets)),
                                    offsets);
            vkCmdDispatch(commandBuffer, groups, 1, 1);
            memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        };

        {
            PassScope scope(*this, commandBuffer, frameIndex, GpuPass::SplatCull);
            dispatch(SortStage::Cull, 0, m_sortTileCount);
        }

        {
            PassScope scope(*this, commandBuffer, frameIndex, GpuPass::SplatDepthSort);
            for (std::uint32_t pass = 0; pass < kSortPasses; ++pass)
            {
                dispatch(SortStage::Histogram, pass, m_sortTileCount);
                dispatch(SortStage::ScanRows, pass, kSortBuckets);
                dispatch(SortStage::ScanTotals, pass, 1);
                dispatch(SortStage::Scatter, pass, m_sortTileCount);
            }
        }

        // Tile binning. The pairs are generated by walking the depth order, so
        // they start out sorted by depth; the tile sort below is stable, which
        // leaves every tile's slice still in depth order.
        {
            PassScope scope(*this, commandBuffer, frameIndex, GpuPass::SplatBinning);
            dispatch(SortStage::BinCount, 0, m_sortTileCount);
            dispatch(SortStage::ScanGroups, 0, m_sortTileCount);
            dispatch(SortStage::ScanGroupSums, 0, 1);
            dispatch(SortStage::ScanAdd, 0, m_sortTileCount);
            dispatch(SortStage::Bin, 0, m_pairTileCount);
        }

        // The per-tile ranges the rasteriser walks come out of the same scope.
        {
            PassScope scope(*this, commandBuffer, frameIndex, GpuPass::SplatTileSort);
            for (std::uint32_t pass = 0; pass < kTileSortPasses; ++pass)
            {
                const std::uint32_t section = kSortPasses + pass;
                dispatch(SortStage::Histogram, section, m_pairTileCount);
                dispatch(SortStage::ScanRows, section, kSortBuckets);
                dispatch(SortStage::ScanTotals, section, 1);
                dispatch(SortStage::Scatter, section, m_pairTileCount);
            }

            const std::uint32_t tilesX = (m_size.x + kSplatTileSize - 1) / kSplatTileSize;
            const std::uint32_t tilesY = (m_size.y + kSplatTileSize - 1) / kSplatTileSize;
            dispatch(SortStage::TileRangesReset, 0,
                     (tilesX * tilesY + kSortGroupSize - 1) / kSortGroupSize);
            dispatch(SortStage::TileRanges, kSortPasses, m_pairTileCount);
        }
    }

    void VulkanRenderer::rasteriseSplats(VkCommandBuffer commandBuffer,
                                        std::uint32_t frameIndex)
    {
        if (m_gaussianCount == 0 || m_sortPipelineLayout == VK_NULL_HANDLE)
        {
            return;
        }

        // Open to the end of the function, so the handover barriers either side of
        // the dispatch are charged to the rasteriser that needs them.
        PassScope scope(*this, commandBuffer, frameIndex, GpuPass::SplatRaster);

        // The mesh pass wrote the depth this reads, and the composite will read
        // the colour this writes.
        imageBarrier(commandBuffer, m_depth.handle,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        imageBarrier(commandBuffer, m_splatColour.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        const std::uint32_t offsets[] = {
            static_cast<std::uint32_t>(frameIndex * m_frameStride), 0u};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_sortPipelines[static_cast<std::size_t>(SortStage::Raster)]);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_sortPipelineLayout, 0, 1, &m_sortSets[frameIndex],
                                static_cast<std::uint32_t>(std::size(offsets)), offsets);

        const std::uint32_t tilesX = (m_size.x + kSplatTileSize - 1) / kSplatTileSize;
        const std::uint32_t tilesY = (m_size.y + kSplatTileSize - 1) / kSplatTileSize;
        vkCmdDispatch(commandBuffer, tilesX, tilesY, 1);

        imageBarrier(commandBuffer, m_splatColour.handle, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        imageBarrier(commandBuffer, m_depth.handle,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    // The rasteriser's depth and colour views change on resize; the sets that
    // name them outlive it, so they are rewritten rather than reallocated.
    void VulkanRenderer::updateSplatImageDescriptors()
    {
        if (m_gaussianCount == 0 || m_splatColour.view == VK_NULL_HANDLE)
        {
            return;
        }

        std::vector<VkWriteDescriptorSet> writes;
        VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, m_depth.view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo storageInfo{VK_NULL_HANDLE, m_splatColour.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo sampledInfo{VK_NULL_HANDLE, m_splatColour.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const auto write = [&](VkDescriptorSet set, std::uint32_t binding,
                               VkDescriptorType type, const VkDescriptorImageInfo* image) {
            if (set == VK_NULL_HANDLE)
            {
                return;
            }
            VkWriteDescriptorSet entry{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            entry.dstSet = set;
            entry.dstBinding = binding;
            entry.descriptorCount = 1;
            entry.descriptorType = type;
            entry.pImageInfo = image;
            writes.push_back(entry);
        };

        for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            write(m_sortSets[i], kSplatDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                  &depthInfo);
            write(m_sortSets[i], kSplatTargetBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                  &storageInfo);
            write(m_tileSortSets[i], kSplatDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                  &depthInfo);
            write(m_tileSortSets[i], kSplatTargetBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                  &storageInfo);
            write(m_splatSets[i], kGaussianBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                  &sampledInfo);
        }

        if (!writes.empty())
        {
            vkUpdateDescriptorSets(m_context.device,
                                   static_cast<std::uint32_t>(writes.size()), writes.data(),
                                   0, nullptr);
        }
    }

    Status VulkanRenderer::createSplatDescriptors()
    {
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight},
        };

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = kFramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
        poolInfo.pPoolSizes = sizes;
        R4D_VK(vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr,
                                      &m_splatDescriptorPool),
               "vkCreateDescriptorPool (splat)");

        const std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{m_splatSetLayout,
                                                                         m_splatSetLayout};
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_splatDescriptorPool;
        allocInfo.descriptorSetCount = kFramesInFlight;
        allocInfo.pSetLayouts = layouts.data();
        R4D_VK(vkAllocateDescriptorSets(m_context.device, &allocInfo, m_splatSets.data()),
               "vkAllocateDescriptorSets (splat)");

        for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            VkDescriptorImageInfo colourInfo{VK_NULL_HANDLE, m_splatColour.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = m_splatSets[i];
            write.dstBinding = kGaussianBinding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &colourInfo;

            vkUpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
        }

        return Status::Ok();
    }

    void VulkanRenderer::renderSplats(VkCommandBuffer commandBuffer,
                                      std::uint32_t frameIndex, std::uint32_t& drawCalls,
                                      std::uint32_t& triangles)
    {
        if (m_gaussianCount == 0 || m_splatPipeline == VK_NULL_HANDLE)
        {
            return;
        }

        PassScope scope(*this, commandBuffer, frameIndex, GpuPass::SplatComposite);

        // The tile rasteriser already composited the splats among themselves;
        // this puts that image over the mesh.
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_splatPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_splatPipelineLayout, 0, 1, &m_splatSets[frameIndex], 0,
                                nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);

        ++drawCalls;
        triangles += 1;
    }

    Status VulkanRenderer::uploadModel(const Model& model)
    {
        if (model.empty() || model.vertices.empty() || model.indices.empty())
        {
            return makeError(ErrorCode::InvalidArgument, "uploadModel called with no geometry");
        }

        waitForGpuIdle();

        destroyBuffer(m_vertexBuffer);
        destroyBuffer(m_indexBuffer);
        destroyBuffer(m_materialUniforms);
        for (Image& texture : m_textures)
        {
            destroyImage(texture);
        }
        m_textures.clear();
        if (m_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_context.device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }

        const VkDeviceSize vertexBytes = model.vertices.size() * sizeof(Vertex);
        const VkDeviceSize indexBytes = model.indices.size() * sizeof(std::uint32_t);

        auto vertexBuffer = createBuffer(vertexBytes,
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "vertex buffer");
        if (!vertexBuffer)
        {
            return vertexBuffer.error();
        }
        m_vertexBuffer = vertexBuffer.value();

        auto indexBuffer = createBuffer(indexBytes,
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "index buffer");
        if (!indexBuffer)
        {
            return indexBuffer.error();
        }
        m_indexBuffer = indexBuffer.value();

        auto vertexStaging = createBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          "vertex staging");
        if (!vertexStaging)
        {
            return vertexStaging.error();
        }
        auto indexStaging = createBuffer(indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         "index staging");
        if (!indexStaging)
        {
            return indexStaging.error();
        }

        std::memcpy(vertexStaging.value().mapped, model.vertices.data(),
                    static_cast<std::size_t>(vertexBytes));
        std::memcpy(indexStaging.value().mapped, model.indices.data(),
                    static_cast<std::size_t>(indexBytes));

        auto commandBuffer = beginOneShot(m_uploadPool);
        if (!commandBuffer)
        {
            return commandBuffer.error();
        }

        VkBufferCopy vertexCopy{0, 0, vertexBytes};
        vkCmdCopyBuffer(commandBuffer.value(), vertexStaging.value().handle,
                        m_vertexBuffer.handle, 1, &vertexCopy);
        VkBufferCopy indexCopy{0, 0, indexBytes};
        vkCmdCopyBuffer(commandBuffer.value(), indexStaging.value().handle,
                        m_indexBuffer.handle, 1, &indexCopy);

        std::vector<Buffer> staging;

        ImageRgba8 white;
        white.width = 1;
        white.height = 1;
        white.rgba = {255, 255, 255, 255};
        ImageRgba8 flatNormal = white;
        flatNormal.rgba = {128, 128, 255, 255};

        auto whiteSrgb = createTexture(white, true, commandBuffer.value(), staging);
        if (!whiteSrgb) return whiteSrgb.error();
        auto whiteLinear = createTexture(white, false, commandBuffer.value(), staging);
        if (!whiteLinear) return whiteLinear.error();
        auto normalDefault = createTexture(flatNormal, false, commandBuffer.value(), staging);
        if (!normalDefault) return normalDefault.error();

        const ImageRgba8& skyImage =
            model.environment.image.empty() ? white : model.environment.image;
        const ImageRgba8& irradianceImage =
            model.irradiance.image.empty() ? white : model.irradiance.image;
        auto sky = createTexture(skyImage, true, commandBuffer.value(), staging);
        if (!sky) return sky.error();
        auto irradiance = createTexture(irradianceImage, true, commandBuffer.value(), staging);
        if (!irradiance) return irradiance.error();

        m_textures.push_back(whiteSrgb.value());
        m_textures.push_back(whiteLinear.value());
        m_textures.push_back(normalDefault.value());
        m_textures.push_back(sky.value());
        m_textures.push_back(irradiance.value());

        std::vector<Image> modelTextures;
        for (const TextureImage& source : model.textures)
        {
            auto created = createTexture(source.image, source.srgb, commandBuffer.value(),
                                         staging);
            if (!created) return created.error();
            modelTextures.push_back(created.value());
            m_textures.push_back(created.value());
        }

        R4D_TRY(endOneShot(m_uploadPool, commandBuffer.value()));

        destroyBuffer(vertexStaging.value());
        destroyBuffer(indexStaging.value());
        for (Buffer& buffer : staging)
        {
            destroyBuffer(buffer);
        }

        m_primitives = model.primitives;
        m_materials = model.materials;
        m_sceneBounds = model.bounds;

        // One uniform slot per material, written once.
        auto materialBuffer = createBuffer(m_materialStride * std::max<std::size_t>(
                                               model.materials.size(), 1),
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                           "material uniforms");
        if (!materialBuffer)
        {
            return materialBuffer.error();
        }
        m_materialUniforms = materialBuffer.value();

        for (std::size_t i = 0; i < model.materials.size(); ++i)
        {
            MaterialConstants constants{};
            constants.baseColour = model.materials[i].baseColour;
            constants.emissive = model.materials[i].emissive;
            constants.metallic = model.materials[i].metallic;
            constants.roughness = model.materials[i].roughness;
            std::memcpy(static_cast<std::byte*>(m_materialUniforms.mapped) +
                            i * m_materialStride,
                        &constants, sizeof(constants));
        }

        R4D_TRY(createDescriptorSets(model));

        R4D_INFO("Geometry: {} vertices, {} triangles, {} primitives, {} textures",
                 model.vertices.size(), model.triangleCount(), model.primitives.size(),
                 model.textures.size());
        return Status::Ok();
    }

    Status VulkanRenderer::createDescriptorSets(const Model& model)
    {
        const auto materialCount = static_cast<std::uint32_t>(
            std::max<std::size_t>(model.materials.size(), 1));

        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, materialCount * 2},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, materialCount * kTexturesPerMaterial},
            {VK_DESCRIPTOR_TYPE_SAMPLER, materialCount * 3},
        };

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = materialCount;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
        poolInfo.pPoolSizes = sizes;
        R4D_VK(vkCreateDescriptorPool(m_context.device, &poolInfo, nullptr, &m_descriptorPool),
               "vkCreateDescriptorPool");

        std::vector<VkDescriptorSetLayout> layouts(materialCount, m_setLayout);
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = materialCount;
        allocInfo.pSetLayouts = layouts.data();

        m_materialSets.assign(materialCount, VK_NULL_HANDLE);
        R4D_VK(vkAllocateDescriptorSets(m_context.device, &allocInfo, m_materialSets.data()),
               "vkAllocateDescriptorSets");

        for (std::uint32_t i = 0; i < materialCount; ++i)
        {
            const Material& material =
                i < model.materials.size() ? model.materials[i] : model.materials.front();

            const auto pick = [&](std::int32_t index, std::size_t fallback) -> VkImageView {
                const std::size_t base = 5;   // defaults and environment occupy 0..4
                if (index >= 0 && base + static_cast<std::size_t>(index) < m_textures.size())
                {
                    return m_textures[base + static_cast<std::size_t>(index)].view;
                }
                return m_textures[fallback].view;
            };

            VkDescriptorBufferInfo frameInfo{m_frameUniforms.handle, 0, sizeof(FrameConstants)};
            VkDescriptorBufferInfo materialInfo{m_materialUniforms.handle, 0,
                                                sizeof(MaterialConstants)};

            VkDescriptorImageInfo images[kTexturesPerMaterial]{};
            images[0].imageView = pick(material.baseColourTexture, 0);
            images[1].imageView = pick(material.metallicRoughnessTexture, 1);
            images[2].imageView = pick(material.normalTexture, 2);
            images[3].imageView = m_textures[3].view;   // environment
            images[4].imageView = m_textures[4].view;   // irradiance
            images[5].imageView = m_shadowMap.view;
            for (VkDescriptorImageInfo& image : images)
            {
                image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            VkDescriptorImageInfo samplerInfo{};
            samplerInfo.sampler = m_sampler;
            VkDescriptorImageInfo environmentSamplerInfo{};
            environmentSamplerInfo.sampler = m_environmentSampler;
            VkDescriptorImageInfo shadowSamplerInfo{};
            shadowSamplerInfo.sampler = m_shadowSampler;

            std::vector<VkWriteDescriptorSet> writes;
            const auto addWrite = [&](std::uint32_t binding, VkDescriptorType type,
                                      const VkDescriptorBufferInfo* buffer,
                                      const VkDescriptorImageInfo* image) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = m_materialSets[i];
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = type;
                write.pBufferInfo = buffer;
                write.pImageInfo = image;
                writes.push_back(write);
            };

            addWrite(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &frameInfo, nullptr);
            addWrite(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &materialInfo, nullptr);
            for (std::uint32_t t = 0; t < kTexturesPerMaterial; ++t)
            {
                addWrite(kTextureBinding + t, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr,
                         &images[t]);
            }
            addWrite(kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, &samplerInfo);
            addWrite(kSamplerBinding + 1, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr,
                     &environmentSamplerInfo);
            addWrite(kSamplerBinding + 2, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr,
                     &shadowSamplerInfo);

            vkUpdateDescriptorSets(m_context.device,
                                   static_cast<std::uint32_t>(writes.size()), writes.data(),
                                   0, nullptr);
        }

        return Status::Ok();
    }

    VkImage VulkanRenderer::currentColourImage() const
    {
        return m_headless ? m_offscreen.handle : m_swapchainImages[m_imageIndex];
    }

    VkImageView VulkanRenderer::currentColourView() const
    {
        return m_headless ? m_offscreen.view : m_swapchainViews[m_imageIndex];
    }

    Status VulkanRenderer::renderFrame()
    {
        if (m_size.x == 0 || m_size.y == 0)
        {
            return Status::Ok();
        }
        if (m_pipeline == VK_NULL_HANDLE)
        {
            return makeError(ErrorCode::InvalidArgument, "renderFrame before initialisation");
        }

        const std::uint32_t frameIndex =
            static_cast<std::uint32_t>(m_frameCounter % kFramesInFlight);
        Frame& frame = m_frames[frameIndex];

        R4D_VK(vkWaitForFences(m_context.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
               "vkWaitForFences");

        // The fence wait above also means this slot's timestamps have landed.
        if (m_timestampPool != VK_NULL_HANDLE && m_timestampWritten[frameIndex])
        {
            std::uint64_t timestamps[2]{};
            if (vkGetQueryPoolResults(m_context.device, m_timestampPool,
                                      frameIndex * kQueriesPerSlot, kFrameQueries,
                                      sizeof(timestamps), timestamps, sizeof(std::uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                timestamps[1] > timestamps[0])
            {
                const double milliseconds =
                    static_cast<double>(timestamps[1] - timestamps[0]) *
                    static_cast<double>(m_context.timestampPeriod) / 1.0e6;
                m_gpuTimeHistory.add(milliseconds);
                m_stats.gpuFrameMs = milliseconds;
                m_stats.gpuFrameAverageMs = m_gpuTimeHistory.average();
                m_stats.gpuTimingsValid = true;
            }
        }

        // All or nothing: the pass pairs are only read when every scope was
        // bracketed, which needs the splat path to have run.
        bool passTimesValid = m_timestampPool != VK_NULL_HANDLE && m_passScopesEnabled &&
                              m_passScopeMask[frameIndex] == kAllPassScopes;
        if (passTimesValid)
        {
            std::array<std::uint64_t, kPassQueries> stamps{};
            if (vkGetQueryPoolResults(m_context.device, m_timestampPool,
                                      frameIndex * kQueriesPerSlot + kFrameQueries,
                                      kPassQueries, stamps.size() * sizeof(std::uint64_t),
                                      stamps.data(), sizeof(std::uint64_t),
                                      VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            {
                passTimesValid = false;
            }
            else
            {
                for (std::size_t pass = 0; pass < kGpuPassCount; ++pass)
                {
                    // Inverted only after a reset; equal is a pass inside one tick.
                    if (stamps[2 * pass + 1] < stamps[2 * pass])
                    {
                        passTimesValid = false;
                        break;
                    }
                    const double milliseconds =
                        static_cast<double>(stamps[2 * pass + 1] - stamps[2 * pass]) *
                        static_cast<double>(m_context.timestampPeriod) / 1.0e6;
                    m_gpuPassHistory[pass].add(milliseconds);
                    m_stats.gpuPassMs[pass] = milliseconds;
                    m_stats.gpuPassAverageMs[pass] = m_gpuPassHistory[pass].average();
                }
            }
        }
        m_stats.gpuPassTimingsValid = passTimesValid;

        if (!m_headless)
        {
            const VkResult acquired =
                vkAcquireNextImageKHR(m_context.device, m_swapchain, UINT64_MAX,
                                      frame.acquired, VK_NULL_HANDLE, &m_imageIndex);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
            {
                m_swapchainDirty = true;
                return resize(m_size);
            }
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            {
                return vulkanError(ErrorCode::ApiCallFailed, acquired, "vkAcquireNextImageKHR");
            }
        }

        R4D_VK(vkResetFences(m_context.device, 1, &frame.inFlight), "vkResetFences");

        const double cpuDelta = m_frameTimer.tick();
        const float timeSeconds =
            (m_fixedTimeStep > 0.0)
                ? static_cast<float>(static_cast<double>(m_frameCounter) * m_fixedTimeStep)
                : static_cast<float>(m_frameTimer.totalSeconds());

        FrameConstants constants{};
        constants.viewProjection = m_camera.projection * m_camera.view;
        constants.lightViewProjection = lightViewProjection();
        constants.view = m_camera.view;
        constants.splatViewport = Vec2{static_cast<float>(m_size.x),
                                       static_cast<float>(m_size.y)};
        // Focal length in pixels, recovered from the projection, which is what
        // the covariance Jacobian needs.
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
        std::memcpy(static_cast<std::byte*>(m_frameUniforms.mapped) +
                        frameIndex * m_frameStride,
                    &constants, sizeof(constants));

        R4D_VK(vkResetCommandPool(m_context.device, frame.commandPool, 0),
               "vkResetCommandPool");

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        R4D_VK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(frame.commandBuffer, m_timestampPool,
                                frameIndex * kQueriesPerSlot, kQueriesPerSlot);
            m_passScopeMask[frameIndex] = 0u;
            vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 m_timestampPool, frameIndex * kQueriesPerSlot);
        }

        // Before anything opens a render pass: the splat order for this frame
        // is produced by compute, and the vertex shader reads it further down.
        sortSplats(frame.commandBuffer, frameIndex);

        renderShadowPass(frame.commandBuffer);

        VkImage colourImage = currentColourImage();

        imageBarrier(frame.commandBuffer, colourImage,
                     m_headless && m_hasRenderedFrame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        imageBarrier(frame.commandBuffer, m_depth.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);

        VkRenderingAttachmentInfo colourAttachment{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colourAttachment.imageView = currentColourView();
        colourAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colourAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(colourAttachment.clearValue.color.float32, kClearColour,
                    sizeof(kClearColour));

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = m_depth.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil.depth = kClearDepth;

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {m_size.x, m_size.y};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colourAttachment;
        rendering.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(frame.commandBuffer, &rendering);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_size.x),
                            static_cast<float>(m_size.y), 0.0f, 1.0f};
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {m_size.x, m_size.y}};
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        std::uint32_t drawCalls = 0;
        std::uint32_t triangles = 0;

        if (!m_primitives.empty() && m_vertexBuffer.handle != VK_NULL_HANDLE)
        {
            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &m_vertexBuffer.handle,
                                   &vertexOffset);
            vkCmdBindIndexBuffer(frame.commandBuffer, m_indexBuffer.handle, 0,
                                 VK_INDEX_TYPE_UINT32);

            for (const Primitive& primitive : m_primitives)
            {
                const std::uint32_t materialIndex =
                    primitive.materialIndex < m_materialSets.size() ? primitive.materialIndex
                                                                    : 0;
                const std::uint32_t offsets[2] = {
                    static_cast<std::uint32_t>(frameIndex * m_frameStride),
                    static_cast<std::uint32_t>(materialIndex * m_materialStride)};

                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_pipelineLayout, 0, 1, &m_materialSets[materialIndex],
                                        2, offsets);

                vkCmdDrawIndexed(frame.commandBuffer, primitive.indexCount, 1,
                                 primitive.firstIndex,
                                 static_cast<std::int32_t>(primitive.vertexOffset), 0);
                ++drawCalls;
                triangles += primitive.indexCount / 3;
            }
        }

        if (m_gaussianCount != 0)
        {
            // Dynamic rendering has to end before a dispatch, and the
            // rasteriser needs the depth the mesh pass just wrote.
            vkCmdEndRendering(frame.commandBuffer);
            rasteriseSplats(frame.commandBuffer, frameIndex);

            VkRenderingAttachmentInfo compositeAttachment = colourAttachment;
            compositeAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            VkRenderingInfo compositeRendering = rendering;
            compositeRendering.pColorAttachments = &compositeAttachment;
            compositeRendering.pDepthAttachment = nullptr;
            vkCmdBeginRendering(frame.commandBuffer, &compositeRendering);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

            renderSplats(frame.commandBuffer, frameIndex, drawCalls, triangles);
        }

        renderUi(frame.commandBuffer);

        vkCmdEndRendering(frame.commandBuffer);

        imageBarrier(frame.commandBuffer, colourImage,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     m_headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_IMAGE_ASPECT_COLOR_BIT);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                 m_timestampPool, frameIndex * kQueriesPerSlot + 1);
            m_timestampWritten[frameIndex] = true;
        }

        R4D_VK(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");

        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = frame.commandBuffer;

        VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitInfo.semaphore = frame.acquired;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalInfo.semaphore =
            m_headless ? VK_NULL_HANDLE : m_renderFinished[m_imageIndex];
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commandInfo;
        if (!m_headless)
        {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &waitInfo;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signalInfo;
        }

        R4D_VK(vkQueueSubmit2(m_context.queue, 1, &submit, frame.inFlight), "vkQueueSubmit2");

        if (!m_headless)
        {
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &m_renderFinished[m_imageIndex];
            present.swapchainCount = 1;
            present.pSwapchains = &m_swapchain;
            present.pImageIndices = &m_imageIndex;

            const VkResult presented = vkQueuePresentKHR(m_context.queue, &present);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
            {
                m_swapchainDirty = true;
            }
            else if (presented != VK_SUCCESS)
            {
                return vulkanError(ErrorCode::ApiCallFailed, presented, "vkQueuePresentKHR");
            }
        }

        ++m_frameCounter;
        m_hasRenderedFrame = true;

        m_stats.cpuFrameMs = cpuDelta * 1000.0;
        m_stats.cpuFrameAverageMs = m_frameTimer.averageMilliseconds();
        m_stats.drawCalls = drawCalls;
        m_stats.dispatches = 0;
        m_stats.triangles = triangles;
        m_stats.renderResolution = m_size;
        m_stats.frameIndex = m_frameCounter;

        return Status::Ok();
    }

    Status VulkanRenderer::resize(UVec2 newSize)
    {
        if (newSize.x == 0 || newSize.y == 0)
        {
            m_size = newSize;
            return Status::Ok();
        }

        waitForGpuIdle();

        if (m_headless)
        {
            R4D_TRY(createOffscreenTarget(newSize));
            R4D_TRY(createSplatTargets(newSize));
            updateSplatImageDescriptors();
        }
        else
        {
            destroySwapchain();
            R4D_TRY(createSwapchain(newSize));
        }
        R4D_TRY(createDepthImage(m_size));
        R4D_TRY(createSplatTargets(m_size));
        updateSplatImageDescriptors();

        m_swapchainDirty = false;
        m_hasRenderedFrame = false;
        R4D_INFO("Resized to {}x{}", m_size.x, m_size.y);
        return Status::Ok();
    }

    void VulkanRenderer::setVsync(bool enabled)
    {
        if (m_vsync == enabled)
        {
            return;
        }
        m_vsync = enabled;
        if (!m_headless)
        {
            // The present mode lives in the swapchain, so it has to be rebuilt.
            waitForGpuIdle();
            destroySwapchain();
            if (auto status = createSwapchain(m_size); !status)
            {
                R4D_ERROR("setVsync: {}", status.error().ToString());
            }
        }
    }

    void VulkanRenderer::waitForGpuIdle()
    {
        if (m_context.device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_context.device);

            shutdownUi();
        }
    }

    Result<FrameCapture> VulkanRenderer::captureFrame()
    {
        if (!m_hasRenderedFrame)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "captureFrame called before any frame was rendered");
        }
        if (!m_headless)
        {
            // After Present the swapchain image is no longer acquired, and
            // transitioning it would be invalid. Capture is a headless path here.
            return makeError(ErrorCode::UnsupportedFeature,
                             "the Vulkan backend can only capture in headless mode; "
                             "use --headless, or --backend d3d12 to capture a window");
        }

        waitForGpuIdle();

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_size.x) * m_size.y * 4;
        auto readback = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     "capture readback");
        if (!readback)
        {
            return readback.error();
        }

        auto commandBuffer = beginOneShot(m_uploadPool);
        if (!commandBuffer)
        {
            destroyBuffer(readback.value());
            return commandBuffer.error();
        }

        VkImage source = currentColourImage();
        const VkImageLayout resting = m_headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        imageBarrier(commandBuffer.value(), source, resting,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {m_size.x, m_size.y, 1};
        vkCmdCopyImageToBuffer(commandBuffer.value(), source,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.value().handle,
                               1, &copy);

        imageBarrier(commandBuffer.value(), source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     resting, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT);

        if (auto status = endOneShot(m_uploadPool, commandBuffer.value()); !status)
        {
            destroyBuffer(readback.value());
            return status.error();
        }

        FrameCapture capture;
        capture.width = m_size.x;
        capture.height = m_size.y;
        capture.rgba.resize(capture.expectedBytes());
        std::memcpy(capture.rgba.data(), readback.value().mapped, capture.expectedBytes());

        destroyBuffer(readback.value());
        return capture;
    }

    VulkanRenderer::~VulkanRenderer()
    {
        if (m_context.device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_context.device);

            if (m_pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_context.device, m_pipeline, nullptr);
            }
            if (m_pipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_context.device, m_pipelineLayout, nullptr);
            }
            if (m_descriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(m_context.device, m_descriptorPool, nullptr);
            }
            if (m_setLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(m_context.device, m_setLayout, nullptr);
            }
            if (m_sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(m_context.device, m_sampler, nullptr);
            }
            if (m_environmentSampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(m_context.device, m_environmentSampler, nullptr);
            }
            if (m_shadowSampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(m_context.device, m_shadowSampler, nullptr);
            }
            if (m_shadowPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_context.device, m_shadowPipeline, nullptr);
            }
            destroySplatBuffers();
            if (m_splatPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_context.device, m_splatPipeline, nullptr);
            }
            if (m_splatPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_context.device, m_splatPipelineLayout, nullptr);
            }
            if (m_splatSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(m_context.device, m_splatSetLayout, nullptr);
            }
            for (VkPipeline pipeline : m_sortPipelines)
            {
                if (pipeline != VK_NULL_HANDLE)
                {
                    vkDestroyPipeline(m_context.device, pipeline, nullptr);
                }
            }
            if (m_sortPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_context.device, m_sortPipelineLayout, nullptr);
            }
            if (m_sortSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(m_context.device, m_sortSetLayout, nullptr);
            }
            if (m_timestampPool != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(m_context.device, m_timestampPool, nullptr);
            }

            for (Image& texture : m_textures)
            {
                destroyImage(texture);
            }
            m_textures.clear();

            destroyBuffer(m_vertexBuffer);
            destroyBuffer(m_indexBuffer);
            destroyBuffer(m_frameUniforms);
            destroyBuffer(m_materialUniforms);

            destroyImage(m_shadowMap);
            destroyImage(m_depth);
            destroyImage(m_splatColour);
            destroyImage(m_offscreen);
            destroySwapchain();

            for (Frame& frame : m_frames)
            {
                if (frame.inFlight != VK_NULL_HANDLE)
                    vkDestroyFence(m_context.device, frame.inFlight, nullptr);
                if (frame.acquired != VK_NULL_HANDLE)
                    vkDestroySemaphore(m_context.device, frame.acquired, nullptr);
                if (frame.commandPool != VK_NULL_HANDLE)
                    vkDestroyCommandPool(m_context.device, frame.commandPool, nullptr);
            }
            if (m_uploadPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_context.device, m_uploadPool, nullptr);
            }
        }

        if (m_surface != VK_NULL_HANDLE && m_context.instance != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(m_context.instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }

        destroyDevice(m_context);
    }

} // namespace r4d::vulkan
