# Warnings-as-errors applies to our own targets only, never to fetched
# dependencies.

function(r4d_set_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /EHsc
            /MP
        )
        if(R4D_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(R4D_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(r4d_set_target_defines target)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )
    endif()
    target_compile_definitions(${target} PRIVATE R4D_MIN_LOG_LEVEL=${R4D_MIN_LOG_LEVEL})
endfunction()
