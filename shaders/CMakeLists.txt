# Build-time HLSL -> DXIL. A shader that fails to compile fails the build.

r4d_add_shader_target(r4d_shaders
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Triangle.vs.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Triangle.ps.hlsl"
)
