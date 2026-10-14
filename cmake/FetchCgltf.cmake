# cgltf: single-header glTF parser. Sets R4D_CGLTF_INCLUDE_DIR.

include(FetchContent)

FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cgltf)

set(R4D_CGLTF_INCLUDE_DIR "${cgltf_SOURCE_DIR}")
if(NOT EXISTS "${R4D_CGLTF_INCLUDE_DIR}/cgltf.h")
    message(FATAL_ERROR "cgltf: cgltf.h not found in ${R4D_CGLTF_INCLUDE_DIR}")
endif()
message(STATUS "cgltf: ${R4D_CGLTF_INCLUDE_DIR}")
