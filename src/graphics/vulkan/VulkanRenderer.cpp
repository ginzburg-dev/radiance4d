// VulkanRenderer.cpp
#include "VulkanRenderer.hpp"

#include <radiance4d/core/FileSystem.hpp>

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
            Vec3 cameraPosition;
            float timeSeconds;
            Vec3 ambient;
            float exposure;
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
        constexpr std::uint32_t kTexturesPerMaterial = 3;

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

    Status VulkanRenderer::createDepthImage(UVec2 size)
    {
        destroyImage(m_depth);
        auto image = createImage(size.x, size.y, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1,
                                 VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!image)
        {
            return image.error();
        }
        m_depth = image.value();
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
        info.queryCount = kFramesInFlight * 2;
        R4D_VK(vkCreateQueryPool(m_context.device, &info, nullptr, &m_timestampPool),
               "vkCreateQueryPool");
        R4D_INFO("Profiler: timestamp queries ready, period {} ns",
                 m_context.timestampPeriod);
        return Status::Ok();
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

        m_textures.push_back(whiteSrgb.value());
        m_textures.push_back(whiteLinear.value());
        m_textures.push_back(normalDefault.value());

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
            {VK_DESCRIPTOR_TYPE_SAMPLER, materialCount},
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
                const std::size_t base = 3;   // defaults occupy slots 0..2
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
            for (VkDescriptorImageInfo& image : images)
            {
                image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            VkDescriptorImageInfo samplerInfo{};
            samplerInfo.sampler = m_sampler;

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
            if (vkGetQueryPoolResults(m_context.device, m_timestampPool, frameIndex * 2, 2,
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
        constants.cameraPosition = m_camera.position;
        constants.timeSeconds = timeSeconds;
        constants.ambient = Vec3{0.055f, 0.065f, 0.09f};
        constants.exposure = 1.0f;
        constants.lights[0].direction = glm::normalize(Vec3{-0.4f, 0.8f, -0.45f});
        constants.lights[0].colour = Vec3{1.0f, 0.96f, 0.9f};
        constants.lights[0].intensity = 3.6f;
        constants.lights[1].direction = glm::normalize(Vec3{0.75f, 0.25f, 0.3f});
        constants.lights[1].colour = Vec3{0.35f, 0.52f, 0.95f};
        constants.lights[1].intensity = 1.5f;
        constants.lights[2].direction = glm::normalize(Vec3{0.1f, -0.6f, 0.75f});
        constants.lights[2].colour = Vec3{0.95f, 0.45f, 0.3f};
        constants.lights[2].intensity = 0.9f;
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
            vkCmdResetQueryPool(frame.commandBuffer, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 m_timestampPool, frameIndex * 2);
        }

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
                                 m_timestampPool, frameIndex * 2 + 1);
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
        }
        else
        {
            destroySwapchain();
            R4D_TRY(createSwapchain(newSize));
        }
        R4D_TRY(createDepthImage(m_size));

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
        }
    }

    Result<FrameCapture> VulkanRenderer::captureFrame()
    {
        if (!m_hasRenderedFrame)
        {
            return makeError(ErrorCode::InvalidArgument,
                             "captureFrame called before any frame was rendered");
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

            destroyImage(m_depth);
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
