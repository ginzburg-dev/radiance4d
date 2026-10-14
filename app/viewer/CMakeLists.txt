# radiance4d_viewer -- the M1 application. Console subsystem on purpose: during
# M1 the log is the user interface and the validation gate reads it.

add_executable(radiance4d_viewer main.cpp)

target_link_libraries(radiance4d_viewer PRIVATE r4d_graphics)

# Agility exports, shader staging and the D3D12 redistributable directory.
r4d_configure_runtime(radiance4d_viewer)

r4d_set_target_warnings(radiance4d_viewer)
r4d_set_target_defines(radiance4d_viewer)
set_target_properties(radiance4d_viewer PROPERTIES FOLDER "Application")
