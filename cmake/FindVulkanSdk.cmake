# Vulkan from the LunarG SDK. Sets R4D_VULKAN_INCLUDE_DIR and R4D_VULKAN_LIBRARY.
#
# The loader (vulkan-1.dll) ships with the driver, but the validation layers only
# come with the SDK, and this project asserts a clean validation run.

set(_sdk "$ENV{VULKAN_SDK}")
if(NOT _sdk)
    file(GLOB _candidates "C:/VulkanSDK/*")
    if(_candidates)
        list(SORT _candidates)
        list(REVERSE _candidates)
        list(GET _candidates 0 _sdk)
    endif()
endif()

if(NOT _sdk OR NOT EXISTS "${_sdk}/Include/vulkan/vulkan.h")
    message(FATAL_ERROR
        "Vulkan SDK not found. Install it from https://vulkan.lunarg.com/sdk/home#windows "
        "or set R4D_ENABLE_VULKAN=OFF. VULKAN_SDK=${_sdk}")
endif()

set(R4D_VULKAN_SDK "${_sdk}")
set(R4D_VULKAN_INCLUDE_DIR "${_sdk}/Include")
set(R4D_VULKAN_LIBRARY "${_sdk}/Lib/vulkan-1.lib")

if(NOT EXISTS "${R4D_VULKAN_LIBRARY}")
    message(FATAL_ERROR "Vulkan SDK: vulkan-1.lib not found at ${R4D_VULKAN_LIBRARY}")
endif()

if(EXISTS "${_sdk}/Bin/VkLayer_khronos_validation.json")
    set(R4D_VULKAN_VALIDATION_AVAILABLE TRUE)
else()
    set(R4D_VULKAN_VALIDATION_AVAILABLE FALSE)
    message(WARNING "Vulkan SDK: validation layer missing; Vulkan runs will not be validated")
endif()

message(STATUS "Vulkan SDK: ${_sdk}")
