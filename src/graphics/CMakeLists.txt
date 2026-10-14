# r4d_graphics -- public headers are API-neutral; the D3D12 implementation is private.
#
# Only this target sees d3d12.h / dxgi.h. IncludeHygiene_test asserts that the
# public headers under include/radiance4d/ never leak them upwards.

set(R4D_GRAPHICS_SOURCES
    AdapterInfo.cpp
    BackendType.cpp
    GpuCapabilities.cpp
    Renderer.cpp
)

if(R4D_ENABLE_D3D12)
    list(APPEND R4D_GRAPHICS_SOURCES
        d3d12/D3D12Adapter.cpp
        d3d12/D3D12Profiler.cpp
        d3d12/D3D12Renderer.cpp
        d3d12/D3D12Validation.cpp
    )
endif()

add_library(r4d_graphics STATIC ${R4D_GRAPHICS_SOURCES})

target_include_directories(r4d_graphics
    PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(r4d_graphics PUBLIC r4d_core)

if(R4D_ENABLE_D3D12)
    target_compile_definitions(r4d_graphics PRIVATE R4D_ENABLE_D3D12=1)
    target_link_libraries(r4d_graphics PRIVATE d3d12 dxgi dxguid)

    if(R4D_ENABLE_AGILITY_SDK)
        # BEFORE matters: the Agility headers must win over the Windows SDK copies,
        # otherwise we compile against older declarations than the runtime we ship.
        target_include_directories(r4d_graphics BEFORE PRIVATE ${R4D_AGILITY_INCLUDE_DIRS})
        target_compile_definitions(r4d_graphics PRIVATE
            R4D_ENABLE_AGILITY_SDK=1
            R4D_AGILITY_SDK_VERSION=${R4D_AGILITY_SDK_VERSION}
        )
    endif()
endif()

r4d_set_target_warnings(r4d_graphics)
r4d_set_target_defines(r4d_graphics)
set_target_properties(r4d_graphics PROPERTIES FOLDER "Engine")
