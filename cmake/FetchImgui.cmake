# Dear ImGui with the Win32 and D3D12 backends. Sets R4D_IMGUI_SOURCES and
# R4D_IMGUI_INCLUDE_DIRS.

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.93.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

set(R4D_IMGUI_INCLUDE_DIRS "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
set(R4D_IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
)
message(STATUS "Dear ImGui: ${imgui_SOURCE_DIR}")
