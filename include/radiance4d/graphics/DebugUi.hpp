// DebugUi.hpp
#pragma once

#include <radiance4d/graphics/GpuStats.hpp>
#include <radiance4d/graphics/RenderSettings.hpp>

#include <string>

namespace r4d
{
    struct AdapterInfo;
    struct GpuCapabilities;

    // Forwards a window message to the UI. Returns true when the UI consumed it,
    // in which case the camera should ignore the input this frame.
    bool debugUiHandleMessage(void* hwnd, unsigned message, unsigned long long wparam,
                              long long lparam);

    bool debugUiWantsMouse();
    bool debugUiWantsKeyboard();

    // Draws the standard statistics window. Called between NewFrame and Render.
    void drawStatsWindow(const GpuStats& stats, const AdapterInfo& adapter,
                         const GpuCapabilities& capabilities, const std::string& backend,
                         bool vsync, const std::string& cameraMode,
                         RenderSettings& settings);
}
