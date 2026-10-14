# Standalone DirectX Shader Compiler.
#
# The Windows SDK copy has its SPIR-V backend stubbed out, so it cannot target
# Vulkan later. Sets R4D_DXC_EXECUTABLE.

set(R4D_DXC_RELEASE "v1.9.2609" CACHE STRING "DirectX Shader Compiler release tag")
set(R4D_DXC_ARCHIVE "dxc_2026_09_29.zip")
set(R4D_DXC_SHA256 "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")

set(_dxc_root "${CMAKE_BINARY_DIR}/_deps/dxc-${R4D_DXC_RELEASE}")
set(_dxc_zip "${_dxc_root}/${R4D_DXC_ARCHIVE}")
set(_dxc_stamp "${_dxc_root}/.extracted")

if(NOT EXISTS "${_dxc_stamp}")
    message(STATUS "DXC: downloading ${R4D_DXC_RELEASE}")
    file(DOWNLOAD
        "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${R4D_DXC_RELEASE}/${R4D_DXC_ARCHIVE}"
        "${_dxc_zip}"
        EXPECTED_HASH SHA256=${R4D_DXC_SHA256}
        TLS_VERIFY ON
        SHOW_PROGRESS
    )
    file(ARCHIVE_EXTRACT INPUT "${_dxc_zip}" DESTINATION "${_dxc_root}")
    file(TOUCH "${_dxc_stamp}")
endif()

set(R4D_DXC_EXECUTABLE "${_dxc_root}/bin/x64/dxc.exe")
if(NOT EXISTS "${R4D_DXC_EXECUTABLE}")
    message(FATAL_ERROR "DXC: dxc.exe not found at ${R4D_DXC_EXECUTABLE}")
endif()
message(STATUS "DXC: ${R4D_DXC_EXECUTABLE}")
