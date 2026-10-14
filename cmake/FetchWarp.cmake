# Pinned WARP software rasteriser.
#
# The in-box WARP differs between Windows versions, so a pinned redistributable
# keeps local and CI image comparisons on the same rasteriser. It is staged next
# to D3D12Core.dll, where the Agility loader finds it.
#
# Sets R4D_WARP_BIN_DIR.

set(R4D_WARP_VERSION "1.0.21" CACHE STRING "Microsoft.Direct3D.WARP version")
set(R4D_WARP_SHA256 "ea44b77eb30eec14427193e20f40cdb2e4c31ed11ab39cb880538fdf4bac2681")

set(_warp_root "${CMAKE_BINARY_DIR}/_deps/warp-${R4D_WARP_VERSION}")
set(_warp_pkg "${_warp_root}/warp.nupkg")
set(_warp_stamp "${_warp_root}/.extracted")

if(NOT EXISTS "${_warp_stamp}")
    message(STATUS "WARP: downloading ${R4D_WARP_VERSION}")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/${R4D_WARP_VERSION}/microsoft.direct3d.warp.${R4D_WARP_VERSION}.nupkg"
        "${_warp_pkg}"
        EXPECTED_HASH SHA256=${R4D_WARP_SHA256}
        TLS_VERIFY ON
        SHOW_PROGRESS
    )
    file(ARCHIVE_EXTRACT INPUT "${_warp_pkg}" DESTINATION "${_warp_root}")
    file(TOUCH "${_warp_stamp}")
endif()

set(R4D_WARP_BIN_DIR "${_warp_root}/build/native/bin/x64")
if(NOT EXISTS "${R4D_WARP_BIN_DIR}/d3d10warp.dll")
    message(FATAL_ERROR "WARP: d3d10warp.dll not found in ${R4D_WARP_BIN_DIR}")
endif()
message(STATUS "WARP: ${R4D_WARP_VERSION} at ${R4D_WARP_BIN_DIR}")
