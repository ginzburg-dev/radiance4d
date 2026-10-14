add_library(r4d_scene STATIC
    Camera.cpp
    DemoScene.cpp
    GltfLoader.cpp
)

target_include_directories(r4d_scene
    PUBLIC "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${R4D_CGLTF_INCLUDE_DIR}"
)
target_link_libraries(r4d_scene PUBLIC r4d_core)

r4d_set_target_warnings(r4d_scene)
r4d_set_target_defines(r4d_scene)
set_target_properties(r4d_scene PROPERTIES FOLDER "Engine")
