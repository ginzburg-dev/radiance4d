// DebugUi.cpp
#include <radiance4d/graphics/DebugUi.hpp>

#include <radiance4d/graphics/AdapterInfo.hpp>
#include <radiance4d/graphics/GpuCapabilities.hpp>

#include <imgui.h>

#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace r4d
{

    bool debugUiHandleMessage(void* hwnd, unsigned message, unsigned long long wparam,
                              long long lparam)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return false;
        }
        if (ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), message,
                                           static_cast<WPARAM>(wparam),
                                           static_cast<LPARAM>(lparam)) != 0)
        {
            return true;
        }

        // Swallow input the UI is using so the camera does not also react to it.
        const ImGuiIO& io = ImGui::GetIO();
        switch (message)
        {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL: return io.WantCaptureMouse;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR: return io.WantCaptureKeyboard;
            default: return false;
        }
    }

    bool debugUiWantsMouse()
    {
        return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    }

    bool debugUiWantsKeyboard()
    {
        return ImGui::GetCurrentContext() != nullptr &&
               ImGui::GetIO().WantCaptureKeyboard;
    }

    void drawStatsWindow(const GpuStats& stats, const AdapterInfo& adapter,
                         const GpuCapabilities& capabilities, const std::string& backend,
                         bool vsync, const std::string& cameraMode,
                         RenderSettings& settings)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Radiance4D", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }

        ImGui::Text("%s  %ux%u  vsync %s", backend.c_str(), stats.renderResolution.x,
                    stats.renderResolution.y, vsync ? "on" : "off");
        ImGui::TextUnformatted(adapter.name.c_str());
        ImGui::Separator();

        ImGui::Text("CPU  %6.3f ms  (avg %6.3f)", stats.cpuFrameMs,
                    stats.cpuFrameAverageMs);
        if (stats.gpuTimingsValid)
        {
            ImGui::Text("GPU  %6.3f ms  (avg %6.3f)", stats.gpuFrameMs,
                        stats.gpuFrameAverageMs);
        }
        else
        {
            // Never show 0.00: the first frames genuinely have no measurement.
            ImGui::TextUnformatted("GPU     --");
        }
        if (stats.gpuPassTimingsValid)
        {
            double splatTotal = 0.0;
            for (std::size_t pass = 0; pass < kGpuPassCount; ++pass)
            {
                const auto name = gpuPassName(static_cast<GpuPass>(pass));
                ImGui::Text("  %-10s %6.3f ms  (avg %6.3f)", name, stats.gpuPassMs[pass],
                            stats.gpuPassAverageMs[pass]);
                splatTotal += stats.gpuPassAverageMs[pass];
            }
            ImGui::Text("  %-10s %6.3f ms  (avg)", "Splats", splatTotal);
        }
        ImGui::Text("Frame %llu", static_cast<unsigned long long>(stats.frameIndex));

        ImGui::Separator();
        ImGui::Text("Draws      %u", stats.drawCalls);
        ImGui::Text("Triangles  %u", stats.triangles);
        ImGui::Text("Camera     %s", cameraMode.c_str());

        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::CollapsingHeader("Controls"))
        {
            ImGui::TextUnformatted("LMB drag    orbit");
            ImGui::TextUnformatted("RMB drag    pan");
            ImGui::TextUnformatted("Wheel       zoom");
            ImGui::TextUnformatted("F2          orbit / free-fly");
            ImGui::TextUnformatted("WASD QE     move, in free-fly");
            ImGui::TextUnformatted("F1          vsync");
            ImGui::TextUnformatted("Esc         quit");
        }

        if (ImGui::CollapsingHeader("Device"))
        {
            ImGui::Text("Feature level  %s", capabilities.featureLevel.c_str());
            ImGui::Text("Shader model   %s", capabilities.highestShaderModel.c_str());
            ImGui::Text("Enhanced barriers  %s",
                        capabilities.enhancedBarriersSupported ? "yes" : "no");
            ImGui::Text("Ray tracing    %s",
                        capabilities.rayTracingSupported ? "yes" : "no");
            ImGui::Text("Mesh shaders   %s",
                        capabilities.meshShadersSupported ? "yes" : "no");
            ImGui::TextWrapped("%s", capabilities.runtimeDescription.c_str());
        }

        ImGui::Separator();
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.02f, 3.0f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Ambient", &settings.ambientIntensity, 0.0f, 2.0f);

        ImGui::End();
    }

} // namespace r4d
