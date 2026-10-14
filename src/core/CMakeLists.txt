# r4d_core -- platform, logging, timing, filesystem and math. No graphics API here.

add_library(r4d_core STATIC
    FileSystem.cpp
    ImageIO.cpp
    Log.cpp
    Result.cpp
    Time.cpp
    Window.cpp
)

target_include_directories(r4d_core
    PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
)

target_include_directories(r4d_core PRIVATE "${R4D_STB_INCLUDE_DIR}")

target_link_libraries(r4d_core
    PUBLIC
        glm::glm
    PRIVATE
        user32
)

r4d_set_target_warnings(r4d_core)
r4d_set_target_defines(r4d_core)
set_target_properties(r4d_core PROPERTIES FOLDER "Engine")
