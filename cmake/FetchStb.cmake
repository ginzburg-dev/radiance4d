# stb_image / stb_image_write, for PNG read and write.
#
# No CMake and no release tags upstream, so a commit is pinned. The
# implementation is compiled once in src/core/ImageIO.cpp.

include(FetchContent)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(stb)

set(R4D_STB_INCLUDE_DIR "${stb_SOURCE_DIR}")
foreach(_header stb_image.h stb_image_write.h)
    if(NOT EXISTS "${R4D_STB_INCLUDE_DIR}/${_header}")
        message(FATAL_ERROR "stb: ${_header} not found in ${R4D_STB_INCLUDE_DIR}")
    endif()
endforeach()
message(STATUS "stb: ${R4D_STB_INCLUDE_DIR}")
