# HLSL to DXIL at build time. The stage comes from the filename infix, e.g.
# Triangle.vs.hlsl. SM 6.0 keeps WARP compatibility for the image tests.

set(R4D_SHADER_MODEL "6_0" CACHE STRING "HLSL shader model")

# Not ${CMAKE_BINARY_DIR}/shaders: CMake generates the shaders/ subproject there.
set(R4D_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders-dxil")

function(r4d_add_shader_target target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "r4d_add_shader_target(${target}): no SOURCES given")
    endif()

    file(MAKE_DIRECTORY "${R4D_SHADER_OUTPUT_DIR}")

    # Any shared header change rebuilds every shader.
    file(GLOB _shader_headers CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*.hlsli")

    set(_outputs "")
    foreach(_src ${ARG_SOURCES})
        get_filename_component(_abs "${_src}" ABSOLUTE)
        get_filename_component(_name "${_src}" NAME)
        string(REGEX REPLACE "\.hlsl$" "" _stem "${_name}")

        if(_stem MATCHES "\.vs$")
            set(_stage "vs")
        elseif(_stem MATCHES "\.ps$")
            set(_stage "ps")
        elseif(_stem MATCHES "\.cs$")
            set(_stage "cs")
        else()
            message(FATAL_ERROR
                "r4d_add_shader_target: cannot infer stage from '${_name}'")
        endif()

        set(_out "${R4D_SHADER_OUTPUT_DIR}/${_stem}.dxil")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${R4D_DXC_EXECUTABLE}"
                    -T ${_stage}_${R4D_SHADER_MODEL}
                    -E main
                    -Fo "${_out}"
                    -I "${CMAKE_SOURCE_DIR}/shaders"
                    -Zpc            # column-major, matching GLM
                    -Zi
                    -Qembed_debug
                    -O3
                    -WX
                    "${_abs}"
            MAIN_DEPENDENCY "${_abs}"
            DEPENDS ${_shader_headers}
            COMMENT "DXC ${_name} -> ${_stem}.dxil (${_stage}_${R4D_SHADER_MODEL})"
            VERBATIM
        )
        list(APPEND _outputs "${_out}")
    endforeach()

    add_custom_target(${target} ALL DEPENDS ${_outputs})
    set_target_properties(${target} PROPERTIES FOLDER "Shaders")
endfunction()
