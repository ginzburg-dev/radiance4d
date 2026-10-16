// VulkanDevice.hpp
#pragma once

#include "VulkanCommon.hpp"

#include <radiance4d/graphics/AdapterInfo.hpp>
#include <radiance4d/graphics/GpuCapabilities.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace r4d::vulkan
{

    // Counts validation messages by severity, mirroring the D3D12 sink so that a
    // run can assert it was clean on either backend.
    class ValidationSink
    {
    public:
        std::uint64_t errorCount() const { return m_errorCount.load(); }
        std::uint64_t warningCount() const { return m_warningCount.load(); }
        void onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                       const char* message);

    private:
        std::atomic<std::uint64_t> m_errorCount{0};
        std::atomic<std::uint64_t> m_warningCount{0};
    };

    struct DeviceContext
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        std::uint32_t queueFamily = 0;

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        VkPhysicalDeviceProperties properties{};
        float timestampPeriod = 0.0f;

        AdapterInfo adapterInfo;
        GpuCapabilities capabilities;
        bool validationEnabled = false;
    };

    Status createInstance(DeviceContext& context, bool enableValidation,
                          ValidationSink* sink);

    // An empty selector takes the first discrete GPU. On a hybrid laptop the
    // integrated device is often enumerated first, and picking it silently would
    // make every later measurement meaningless.
    Status selectPhysicalDevice(DeviceContext& context, const std::string& selector);

    Status createLogicalDevice(DeviceContext& context, VkSurfaceKHR surface);

    void destroyDevice(DeviceContext& context);

    std::uint32_t findMemoryType(const DeviceContext& context, std::uint32_t typeBits,
                                 VkMemoryPropertyFlags properties);

} // namespace r4d::vulkan
