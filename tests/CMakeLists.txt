# M1-B test suite.
#
# Everything here runs without a discrete GPU: the device tests use the pinned
# WARP software rasteriser, which is what lets CI run them on a hosted runner.
# Tests are labelled so that a future GPU-only suite can be excluded with
# `ctest -LE gpu`.

# Pure CPU tests: no D3D12 device, no runtime staging needed.
set(R4D_UNIT_TESTS
    Math_test.cpp
    Log_test.cpp
    Result_test.cpp
    RollingAverage_test.cpp
    ImageIO_test.cpp
    IncludeHygiene_test.cpp
)

# Tests that create a real D3D12 device on WARP.
set(R4D_DEVICE_TESTS
    D3D12Device_test.cpp
    HeadlessRender_test.cpp
)

# Needs a real GPU: there is no software Vulkan device the way WARP covers D3D12.
set(R4D_GPU_TESTS
    BackendParity_test.cpp
)

function(r4d_add_test test_src needs_device)
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE r4d_graphics r4d_scene gtest_main)
    target_compile_definitions(${test_name} PRIVATE
        R4D_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    )

    # /W4 but NOT /WX here: GoogleTest macros are not warning-clean under /W4 and
    # failing the build on a dependency is not useful.
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /W4 /utf-8 /EHsc /Zc:__cplusplus)
    endif()
    r4d_set_target_defines(${test_name})
    set_target_properties(${test_name} PROPERTIES FOLDER "Tests")

    if(needs_device STREQUAL "gpu")
        r4d_configure_runtime(${test_name})
        set(test_labels "gpu")
    elseif(needs_device)
        # Agility exports and the staged D3D12/ directory, without which the test
        # would quietly run against the OS runtime and a different WARP.
        r4d_configure_runtime(${test_name})
        set(test_labels "warp")
    else()
        set(test_labels "unit")
    endif()

    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "${test_labels}")
endfunction()

foreach(test_src ${R4D_UNIT_TESTS})
    r4d_add_test(${test_src} FALSE)
endforeach()

foreach(test_src ${R4D_DEVICE_TESTS})
    r4d_add_test(${test_src} TRUE)
endforeach()

foreach(test_src ${R4D_GPU_TESTS})
    r4d_add_test(${test_src} "gpu")
endforeach()
