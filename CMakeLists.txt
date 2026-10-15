cmake_minimum_required(VERSION 3.26)

project(Radiance4D
    VERSION 0.1.0
    DESCRIPTION "Hybrid real-time rendering and spatiotemporal reconstruction engine"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR
        "Radiance4D currently targets Windows only (D3D12 backend). "
        "A Vulkan backend arrives at M3.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(R4D_ENABLE_D3D12        "Build the Direct3D 12 backend"             ON)
option(R4D_ENABLE_VULKAN       "Build the Vulkan backend (M3, not yet implemented)" OFF)
option(R4D_BUILD_TESTS         "Build tests"                               ON)
option(R4D_ENABLE_AGILITY_SDK  "Use the redistributable DirectX Agility SDK" ON)
option(R4D_WARNINGS_AS_ERRORS  "Treat compiler warnings as errors"          ON)
set(R4D_MIN_LOG_LEVEL "0" CACHE STRING "Compile-time minimum log level (0=Trace)")

if(R4D_ENABLE_VULKAN)
    message(FATAL_ERROR
        "R4D_ENABLE_VULKAN is not implemented yet (planned for M3) and no Vulkan SDK "
        "is installed on this machine. Leave it OFF.")
endif()

# Single output tree, matching the sibling denoise-machine-x project.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(R4DWarnings)
include(FetchDXC)
include(FetchStb)
include(FetchCgltf)
include(FetchImgui)
include(FetchWarp)
include(R4DShaders)
include(R4DRuntimeFiles)
if(R4D_ENABLE_AGILITY_SDK)
    include(FetchAgilitySDK)
endif()

set(R4D_AGILITY_EXPORTS_SOURCE "${CMAKE_SOURCE_DIR}/src/graphics/AgilitySdkExports.cpp")

include(FetchContent)

# GLM: API-neutral math. Depth range and handedness are pinned here so the
# convention lives in exactly one place (see include/radiance4d/core/Math.hpp).
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)
target_compile_definitions(glm INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # D3D12/Vulkan clip space, not OpenGL's [-1,1]
    GLM_FORCE_LEFT_HANDED
    GLM_FORCE_XYZW_ONLY
    GLM_ENABLE_EXPERIMENTAL
)

configure_file(
    "${CMAKE_SOURCE_DIR}/include/radiance4d/core/Version.hpp.in"
    "${CMAKE_BINARY_DIR}/generated/radiance4d/core/Version.hpp"
    @ONLY
)

add_subdirectory(src/core)
add_subdirectory(src/scene)
add_subdirectory(src/graphics)
add_subdirectory(shaders)
add_subdirectory(app/viewer)

if(R4D_BUILD_TESTS)
    include(CTest)
    enable_testing()
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "Radiance4D ${PROJECT_VERSION}")
message(STATUS "  C++ standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  D3D12 backend     : ${R4D_ENABLE_D3D12}")
message(STATUS "  Agility SDK       : ${R4D_ENABLE_AGILITY_SDK}")
message(STATUS "  Tests             : ${R4D_BUILD_TESTS}")
message(STATUS "  Warnings as errors: ${R4D_WARNINGS_AS_ERRORS}")
message(STATUS "")
