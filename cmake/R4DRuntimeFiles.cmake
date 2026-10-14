# Shaders and the D3D12 redistributable, staged next to an executable.
#
# The Agility exports have to be compiled into every executable that creates a
# device, including the test binaries, or the loader falls back to the OS
# runtime.

function(r4d_configure_runtime target)
    target_compile_definitions(${target} PRIVATE
        R4D_SHADER_DIR="${R4D_SHADER_OUTPUT_DIR}"
    )

    if(R4D_ENABLE_AGILITY_SDK)
        target_sources(${target} PRIVATE "${R4D_AGILITY_EXPORTS_SOURCE}")
        target_compile_definitions(${target} PRIVATE
            R4D_ENABLE_AGILITY_SDK=1
            R4D_AGILITY_SDK_VERSION=${R4D_AGILITY_SDK_VERSION}
        )
    endif()

    add_dependencies(${target} r4d_shaders)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${R4D_SHADER_OUTPUT_DIR}"
                "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMENT "Staging compiled shaders beside ${target}"
        VERBATIM
    )

    if(R4D_ENABLE_AGILITY_SDK)
        # The debug layer DLL must match D3D12Core.dll or it fails to load.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/D3D12"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${R4D_AGILITY_BIN_DIR}/D3D12Core.dll"
                    "${R4D_AGILITY_BIN_DIR}/d3d12SDKLayers.dll"
                    "${R4D_WARP_BIN_DIR}/d3d10warp.dll"
                    "$<TARGET_FILE_DIR:${target}>/D3D12/"
            COMMENT "Staging the Agility SDK and WARP beside ${target}"
            VERBATIM
        )
    endif()
endfunction()
