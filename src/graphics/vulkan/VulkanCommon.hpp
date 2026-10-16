// VulkanCommon.hpp
#pragma once

#include <radiance4d/core/Log.hpp>
#include <radiance4d/core/Result.hpp>

#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan.h>

#include <format>
#include <string>

namespace r4d::vulkan
{

    std::string resultToString(VkResult result);

    inline Error vulkanError(ErrorCode code, VkResult result, std::string_view what)
    {
        return makeError(code, std::format("{}: {}", what, resultToString(result)),
                         static_cast<long>(result));
    }

} // namespace r4d::vulkan

#define R4D_VK(expr, what)                                                               \
    do                                                                                   \
    {                                                                                    \
        const VkResult _r4d_vk = (expr);                                                 \
        if (_r4d_vk != VK_SUCCESS)                                                       \
        {                                                                                \
            return ::r4d::vulkan::vulkanError(::r4d::ErrorCode::ApiCallFailed, _r4d_vk,  \
                                              (what));                                   \
        }                                                                                \
    } while (false)

#define R4D_VK_DEVICE(expr, what)                                                        \
    do                                                                                   \
    {                                                                                    \
        const VkResult _r4d_vk = (expr);                                                 \
        if (_r4d_vk != VK_SUCCESS)                                                       \
        {                                                                                \
            return ::r4d::vulkan::vulkanError(::r4d::ErrorCode::DeviceCreationFailed,    \
                                              _r4d_vk, (what));                          \
        }                                                                                \
    } while (false)
