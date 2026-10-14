# DirectX Agility SDK (retail).
#
# Preview releases require Windows Developer Mode, so the newest retail release
# is used. Enhanced Barriers arrived in 1.608.0.
#
# Sets R4D_AGILITY_INCLUDE_DIRS, R4D_AGILITY_BIN_DIR, R4D_AGILITY_SDK_VERSION.

set(R4D_AGILITY_VERSION "1.619.6" CACHE STRING "DirectX Agility SDK (retail) version")
set(R4D_AGILITY_SHA256 "08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9")

set(_agility_root "${CMAKE_BINARY_DIR}/_deps/agility-${R4D_AGILITY_VERSION}")
set(_agility_pkg  "${_agility_root}/agility.nupkg")
set(_agility_stamp "${_agility_root}/.extracted")

if(NOT EXISTS "${_agility_stamp}")
    message(STATUS "Agility SDK: downloading retail ${R4D_AGILITY_VERSION}")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${R4D_AGILITY_VERSION}/microsoft.direct3d.d3d12.${R4D_AGILITY_VERSION}.nupkg"
        "${_agility_pkg}"
        EXPECTED_HASH SHA256=${R4D_AGILITY_SHA256}
        TLS_VERIFY ON
        SHOW_PROGRESS
    )
    file(ARCHIVE_EXTRACT INPUT "${_agility_pkg}" DESTINATION "${_agility_root}")
    file(TOUCH "${_agility_stamp}")
endif()

set(R4D_AGILITY_INCLUDE_DIRS
    "${_agility_root}/build/native/include"
    "${_agility_root}/build/native/include/d3dx12"
)
set(R4D_AGILITY_BIN_DIR "${_agility_root}/build/native/bin/x64")

foreach(_dir IN LISTS R4D_AGILITY_INCLUDE_DIRS)
    if(NOT EXISTS "${_dir}")
        message(FATAL_ERROR "Agility SDK: missing after extraction: ${_dir}")
    endif()
endforeach()
if(NOT EXISTS "${R4D_AGILITY_BIN_DIR}")
    message(FATAL_ERROR "Agility SDK: missing after extraction: ${R4D_AGILITY_BIN_DIR}")
endif()
foreach(_dll D3D12Core.dll d3d12SDKLayers.dll)
    if(NOT EXISTS "${R4D_AGILITY_BIN_DIR}/${_dll}")
        message(FATAL_ERROR "Agility SDK: ${_dll} not found in ${R4D_AGILITY_BIN_DIR}")
    endif()
endforeach()

# Read the version from the shipped header so the export cannot drift from the
# redistributed DLL. The macro name contains digits, so anchor on it.
file(STRINGS "${_agility_root}/build/native/include/d3d12.h" _sdkver_line
     REGEX "^#define[ \t]+D3D12_SDK_VERSION")
if(_sdkver_line MATCHES "D3D12_SDK_VERSION[^0-9]+([0-9]+)")
    set(R4D_AGILITY_SDK_VERSION "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Agility SDK: could not parse D3D12_SDK_VERSION from d3d12.h")
endif()
message(STATUS "Agility SDK: ${R4D_AGILITY_VERSION}, D3D12_SDK_VERSION=${R4D_AGILITY_SDK_VERSION}")
