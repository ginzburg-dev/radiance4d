// VulkanDevice.cpp
#include "VulkanDevice.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace r4d::vulkan
{
    namespace
    {
        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

        const char* kDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        std::string toLower(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            std::transform(text.begin(), text.end(), std::back_inserter(out),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        VKAPI_ATTR VkBool32 VKAPI_CALL
        debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                      VkDebugUtilsMessageTypeFlagsEXT,
                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
        {
            if (auto* sink = static_cast<ValidationSink*>(userData); sink != nullptr)
            {
                sink->onMessage(severity, data != nullptr ? data->pMessage : nullptr);
            }
            return VK_FALSE;
        }

        bool layerAvailable(const char* name)
        {
            std::uint32_t count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> layers(count);
            vkEnumerateInstanceLayerProperties(&count, layers.data());
            return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) {
                return std::strcmp(l.layerName, name) == 0;
            });
        }

        const char* deviceTypeName(VkPhysicalDeviceType type)
        {
            switch (type)
            {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
                case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
                default:                                     return "other";
            }
        }

        std::string apiVersionString(std::uint32_t version)
        {
            return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version),
                               VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
        }
    } // namespace

    std::string resultToString(VkResult result)
    {
        switch (result)
        {
            case VK_SUCCESS:                        return "VK_SUCCESS";
            case VK_NOT_READY:                      return "VK_NOT_READY";
            case VK_TIMEOUT:                        return "VK_TIMEOUT";
            case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
            case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
            default:
                return std::format("VkResult {}", static_cast<int>(result));
        }
    }

    void ValidationSink::onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                   const char* message)
    {
        const char* text = (message != nullptr) ? message : "(no message)";
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
        {
            m_errorCount.fetch_add(1);
            R4D_ERROR("Vulkan ERROR: {}", text);
        }
        else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
        {
            m_warningCount.fetch_add(1);
            R4D_WARN("Vulkan WARNING: {}", text);
        }
        else
        {
            R4D_DEBUG("Vulkan: {}", text);
        }
    }

    Status createInstance(DeviceContext& context, bool enableValidation, ValidationSink* sink)
    {
        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName = "Radiance4D";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Radiance4D";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        // 1.3 for dynamic rendering and synchronization2 in core.
        appInfo.apiVersion = VK_API_VERSION_1_3;

        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        std::vector<const char*> layers;

        const bool validationWanted = enableValidation && layerAvailable(kValidationLayer);
        if (enableValidation && !validationWanted)
        {
            R4D_WARN("Vulkan: {} not available; this run will NOT be validated",
                     kValidationLayer);
        }
        if (validationWanted)
        {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &appInfo;
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        info.ppEnabledLayerNames = layers.data();

        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = &debugCallback;
        messengerInfo.pUserData = sink;

        if (validationWanted)
        {
            // Chained so that messages from instance creation itself are caught.
            info.pNext = &messengerInfo;
        }

        R4D_VK_DEVICE(vkCreateInstance(&info, nullptr, &context.instance), "vkCreateInstance");

        if (validationWanted)
        {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(context.instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create != nullptr)
            {
                R4D_VK(create(context.instance, &messengerInfo, nullptr, &context.messenger),
                       "vkCreateDebugUtilsMessengerEXT");
                context.validationEnabled = true;
                R4D_INFO("Vulkan: validation layer active, messages are counted");
            }
        }

        return Status::Ok();
    }

    Status selectPhysicalDevice(DeviceContext& context, const std::string& selector)
    {
        std::uint32_t count = 0;
        R4D_VK_DEVICE(vkEnumeratePhysicalDevices(context.instance, &count, nullptr),
                      "vkEnumeratePhysicalDevices");
        if (count == 0)
        {
            return makeError(ErrorCode::DeviceCreationFailed,
                             "no Vulkan physical devices were enumerated");
        }
        std::vector<VkPhysicalDevice> devices(count);
        R4D_VK_DEVICE(vkEnumeratePhysicalDevices(context.instance, &count, devices.data()),
                      "vkEnumeratePhysicalDevices");

        const std::string wanted = toLower(selector);
        VkPhysicalDevice chosen = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties chosenProperties{};

        for (std::uint32_t i = 0; i < count; ++i)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            R4D_INFO("Vulkan device[{}]: {} ({}, API {})", i, properties.deviceName,
                     deviceTypeName(properties.deviceType),
                     apiVersionString(properties.apiVersion));

            bool matches = false;
            if (wanted.empty())
            {
                matches = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            }
            else if (std::all_of(wanted.begin(), wanted.end(), [](unsigned char c) {
                         return std::isdigit(c) != 0;
                     }))
            {
                std::uint32_t index = 0;
                std::from_chars(wanted.data(), wanted.data() + wanted.size(), index);
                matches = (index == i);
            }
            else
            {
                matches = toLower(properties.deviceName).find(wanted) != std::string::npos;
            }

            if (matches && chosen == VK_NULL_HANDLE)
            {
                chosen = devices[i];
                chosenProperties = properties;
            }
        }

        if (chosen == VK_NULL_HANDLE)
        {
            if (wanted.empty())
            {
                return makeError(ErrorCode::DeviceCreationFailed,
                                 "no discrete GPU found; pass --adapter with an index or "
                                 "name to use another device");
            }
            return makeError(ErrorCode::NotFound,
                             std::format("no Vulkan device matched '{}'", selector));
        }

        context.physicalDevice = chosen;
        context.properties = chosenProperties;
        context.timestampPeriod = chosenProperties.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(chosen, &context.memoryProperties);

        AdapterInfo& info = context.adapterInfo;
        info.name = chosenProperties.deviceName;
        info.vendorId = chosenProperties.vendorID;
        info.deviceId = chosenProperties.deviceID;
        info.isSoftware = chosenProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;

        for (std::uint32_t i = 0; i < context.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap& heap = context.memoryProperties.memoryHeaps[i];
            if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            {
                info.dedicatedVideoMemory += heap.size;
            }
            else
            {
                info.sharedSystemMemory += heap.size;
            }
        }

        R4D_INFO("Vulkan: SELECTED {}", info.ToString());
        return Status::Ok();
    }

    Status createLogicalDevice(DeviceContext& context, VkSurfaceKHR surface)
    {
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &familyCount,
                                                 families.data());

        bool found = false;
        for (std::uint32_t i = 0; i < familyCount; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
            {
                continue;
            }
            if (surface != VK_NULL_HANDLE)
            {
                VkBool32 presentSupported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(context.physicalDevice, i, surface,
                                                     &presentSupported);
                if (presentSupported != VK_TRUE)
                {
                    continue;
                }
            }
            context.queueFamily = i;
            found = true;
            break;
        }

        if (!found)
        {
            return makeError(ErrorCode::DeviceCreationFailed,
                             "no queue family supports graphics and presentation");
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = context.queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        // Both are core in 1.3 but still have to be switched on explicitly.
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(context.physicalDevice, &features2);

        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE)
        {
            return makeError(ErrorCode::UnsupportedFeature,
                             "this device does not report dynamicRendering and "
                             "synchronization2, which the Vulkan backend requires");
        }

        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.samplerAnisotropy =
            features2.features.samplerAnisotropy;
        enabledFeatures.fillModeNonSolid = features2.features.fillModeNonSolid;

        VkPhysicalDeviceVulkan13Features enable13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enable13.dynamicRendering = VK_TRUE;
        enable13.synchronization2 = VK_TRUE;

        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enable13;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(std::size(kDeviceExtensions));
        deviceInfo.ppEnabledExtensionNames = kDeviceExtensions;
        deviceInfo.pEnabledFeatures = &enabledFeatures;

        R4D_VK_DEVICE(vkCreateDevice(context.physicalDevice, &deviceInfo, nullptr,
                                     &context.device),
                      "vkCreateDevice");
        vkGetDeviceQueue(context.device, context.queueFamily, 0, &context.queue);

        GpuCapabilities& caps = context.capabilities;
        caps.featureLevel = apiVersionString(context.properties.apiVersion);
        caps.highestShaderModel = "SPIR-V 1.6";
        // Vulkan 1.3 has synchronization2 in core, which is the equivalent of the
        // Enhanced Barriers the D3D12 backend requires.
        caps.enhancedBarriersSupported = true;
        caps.tearingSupported = false;
        caps.resourceBindingTier = 0;
        caps.resourceHeapTier = 0;
        caps.rayTracingSupported = false;
        caps.meshShadersSupported = false;
        caps.runtimeDescription =
            std::format("Vulkan {} on {} (driver {})",
                        apiVersionString(context.properties.apiVersion),
                        context.properties.deviceName, context.properties.driverVersion);

        R4D_INFO("Vulkan: device created, queue family {}, timestamp period {} ns",
                 context.queueFamily, context.timestampPeriod);
        return Status::Ok();
    }

    void destroyDevice(DeviceContext& context)
    {
        if (context.device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(context.device, nullptr);
            context.device = VK_NULL_HANDLE;
        }
        if (context.messenger != VK_NULL_HANDLE && context.instance != VK_NULL_HANDLE)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(context.instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr)
            {
                destroy(context.instance, context.messenger, nullptr);
            }
            context.messenger = VK_NULL_HANDLE;
        }
        if (context.instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(context.instance, nullptr);
            context.instance = VK_NULL_HANDLE;
        }
    }

    std::uint32_t findMemoryType(const DeviceContext& context, std::uint32_t typeBits,
                                 VkMemoryPropertyFlags properties)
    {
        for (std::uint32_t i = 0; i < context.memoryProperties.memoryTypeCount; ++i)
        {
            const bool typeAllowed = (typeBits & (1u << i)) != 0;
            const bool hasProperties =
                (context.memoryProperties.memoryTypes[i].propertyFlags & properties) ==
                properties;
            if (typeAllowed && hasProperties)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

} // namespace r4d::vulkan
