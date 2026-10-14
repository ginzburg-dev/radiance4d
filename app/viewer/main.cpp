// main.cpp
//
// Console application: the log is the interface until the ImGui overlay lands.

#include <radiance4d/core/FileSystem.hpp>
#include <radiance4d/core/ImageIO.hpp>
#include <radiance4d/core/Log.hpp>
#include <radiance4d/core/Version.hpp>
#include <radiance4d/core/Window.hpp>
#include <radiance4d/graphics/Renderer.hpp>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct Options
    {
        r4d::BackendType backend = r4d::BackendType::D3D12;
        std::string adapterSelector;
        std::uint32_t width = 1600;
        std::uint32_t height = 900;
        bool vsync = true;
        bool debugLayer = true;
        bool gpuValidation = false;
        int logLevel = 2;
        std::uint64_t frameLimit = 0;
        std::uint32_t resizeCycles = 0;
        bool headless = false;
        std::string outputPath;
        double fixedTimeStep = 0.0;
        bool help = false;
    };

    void printUsage()
    {
        std::printf(
            "Radiance4D viewer %s\n\n"
            "  --adapter <sel>      empty = fastest hardware adapter, \"warp\",\n"
            "                       an index, or part of an adapter name\n"
            "  --width <n> --height <n>\n"
            "  --vsync / --no-vsync\n"
            "  --debug / --no-debug         D3D12 debug layer (default: on)\n"
            "  --gpu-validation             GPU-based validation (slow; never benchmark)\n"
            "  --log-level <0..5>           0=trace 2=info 4=error\n"
            "  --frames <n>                 exit after n frames (for automated runs)\n"
            "  --resize-cycles <n>          cycle through window sizes n times\n"
            "  --headless                   no window; render to an offscreen target\n"
            "  --out <file.png>             write the last rendered frame as a PNG\n"
            "  --fixed-step <seconds>       advance animation by a fixed amount per\n"
            "                               frame instead of using the wall clock,\n"
            "                               which is what makes a capture reproducible\n"
            "  --help\n\n"
            "Exit code is non-zero if any debug-layer error was reported.\n",
            std::string{r4d::kVersionString}.c_str());
    }

    bool parseUint(std::string_view text, std::uint64_t& out)
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }

    bool parseOptions(int argc, char** argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg{argv[i]};
            const auto next = [&](std::string_view& out) {
                if (i + 1 >= argc) return false;
                out = argv[++i];
                return true;
            };

            std::string_view value;
            if (arg == "--help" || arg == "-h")
            {
                options.help = true;
            }
            else if (arg == "--adapter")
            {
                if (!next(value)) return false;
                options.adapterSelector = std::string{value};
            }
            else if (arg == "--width")
            {
                std::uint64_t n = 0;
                if (!next(value) || !parseUint(value, n) || n == 0) return false;
                options.width = static_cast<std::uint32_t>(n);
            }
            else if (arg == "--height")
            {
                std::uint64_t n = 0;
                if (!next(value) || !parseUint(value, n) || n == 0) return false;
                options.height = static_cast<std::uint32_t>(n);
            }
            else if (arg == "--vsync")      { options.vsync = true; }
            else if (arg == "--no-vsync")   { options.vsync = false; }
            else if (arg == "--debug")      { options.debugLayer = true; }
            else if (arg == "--no-debug")   { options.debugLayer = false; }
            else if (arg == "--gpu-validation") { options.gpuValidation = true; }
            else if (arg == "--headless")   { options.headless = true; }
            else if (arg == "--log-level")
            {
                std::uint64_t n = 0;
                if (!next(value) || !parseUint(value, n) || n > 5) return false;
                options.logLevel = static_cast<int>(n);
            }
            else if (arg == "--frames")
            {
                if (!next(value) || !parseUint(value, options.frameLimit)) return false;
            }
            else if (arg == "--resize-cycles")
            {
                std::uint64_t n = 0;
                if (!next(value) || !parseUint(value, n)) return false;
                options.resizeCycles = static_cast<std::uint32_t>(n);
            }
            else if (arg == "--out")
            {
                if (!next(value)) return false;
                options.outputPath = std::string{value};
            }
            else if (arg == "--fixed-step")
            {
                if (!next(value)) return false;
                try
                {
                    options.fixedTimeStep = std::stod(std::string{value});
                }
                catch (const std::exception&)
                {
                    return false;
                }
                if (options.fixedTimeStep < 0.0) return false;
            }
            else if (arg == "--backend")
            {
                if (!next(value)) return false;
                if (!r4d::parseBackendType(value, options.backend)) return false;
            }
            else
            {
                std::printf("unknown argument: %.*s\n", static_cast<int>(arg.size()),
                            arg.data());
                return false;
            }
        }
        return true;
    }

    r4d::RendererDesc makeRendererDesc(const Options& options, void* windowHandle,
                                       r4d::UVec2 size)
    {
        r4d::RendererDesc desc;
        desc.windowHandle = windowHandle;
        desc.initialSize = size;
        desc.adapterSelector = options.adapterSelector;
        desc.enableDebugLayer = options.debugLayer;
        desc.enableGpuValidation = options.gpuValidation;
        desc.vsync = options.vsync;
        desc.fixedTimeStep = options.fixedTimeStep;
        return desc;
    }

    // Returns false on failure.
    bool writeCaptureIfRequested(r4d::Renderer& gpu, const Options& options)
    {
        if (options.outputPath.empty())
        {
            return true;
        }

        auto capture = gpu.captureFrame();
        if (!capture)
        {
            R4D_ERROR("Capture failed: {}", capture.error().ToString());
            return false;
        }
        if (auto status = r4d::writePng(options.outputPath, capture.value()); !status)
        {
            R4D_ERROR("Writing '{}' failed: {}", options.outputPath,
                      status.error().ToString());
            return false;
        }
        R4D_INFO("Wrote {}x{} capture to '{}'", capture.value().width,
                 capture.value().height, options.outputPath);
        return true;
    }

    void logFrameSummary(const r4d::Renderer& gpu, std::uint64_t frames)
    {
        const auto& stats = gpu.stats();
        if (stats.gpuTimingsValid)
        {
            R4D_INFO("Shutdown after {} frames. CPU {:.3f} ms (avg {:.3f}), "
                     "GPU {:.3f} ms (avg {:.3f})",
                     frames, stats.cpuFrameMs, stats.cpuFrameAverageMs, stats.gpuFrameMs,
                     stats.gpuFrameAverageMs);
        }
        else
        {
            R4D_INFO("Shutdown after {} frames. CPU {:.3f} ms (avg {:.3f}). GPU timings "
                     "unavailable.",
                     frames, stats.cpuFrameMs, stats.cpuFrameAverageMs);
        }
    }

    // Contributes to the process exit code.
    int reportValidation(const r4d::Renderer& gpu, const Options& options,
                         std::uint64_t frames)
    {
        const std::uint64_t validationErrors = gpu.validationErrorCount();
        if (validationErrors != 0)
        {
            R4D_CRIT("VALIDATION FAILED: {} debug-layer error(s) or corruption(s) reported",
                     validationErrors);
            return 1;
        }
        if (options.debugLayer)
        {
            R4D_INFO("Validation clean: 0 debug-layer errors across {} frames", frames);
        }
        else
        {
            R4D_WARN("Debug layer was disabled; this run proves nothing about validation");
        }
        return 0;
    }

    int runHeadless(const Options& optionsIn)
    {
        using namespace r4d;

        Options options = optionsIn;
        if (options.frameLimit == 0)
        {
            // Nothing to close, so a headless run must be bounded.
            options.frameLimit = 1;
            R4D_INFO("Headless: no --frames given, rendering a single frame");
        }
        if (options.fixedTimeStep == 0.0 && !options.outputPath.empty())
        {
            R4D_WARN("Headless capture without --fixed-step: the animation follows the "
                     "wall clock, so the output is not reproducible");
        }

        auto renderer = createRenderer(options.backend,
                                       makeRendererDesc(options, nullptr,
                                                        UVec2{options.width, options.height}));
        if (!renderer)
        {
            R4D_CRIT("Renderer creation failed: {}", renderer.error().ToString());
            return 1;
        }
        auto& gpu = *renderer.value();

        int exitCode = 0;
        std::uint64_t frame = 0;
        for (; frame < options.frameLimit; ++frame)
        {
            if (auto status = gpu.renderFrame(); !status)
            {
                R4D_CRIT("renderFrame failed: {}", status.error().ToString());
                exitCode = 1;
                break;
            }
        }

        if (exitCode == 0 && !writeCaptureIfRequested(gpu, options))
        {
            exitCode = 1;
        }

        logFrameSummary(gpu, frame);
        exitCode |= reportValidation(gpu, options, frame);

        renderer.value().reset();
        return exitCode;
    }

    int runWindowed(const Options& options)
    {
        using namespace r4d;

        WindowDesc windowDesc;
        windowDesc.width = options.width;
        windowDesc.height = options.height;
        windowDesc.title = "Radiance4D -- M1-B (D3D12)";

        auto window = Window::create(windowDesc);
        if (!window)
        {
            R4D_CRIT("Window creation failed: {}", window.error().ToString());
            return 1;
        }

        auto renderer = createRenderer(
            options.backend,
            makeRendererDesc(options, window.value()->nativeHandle(),
                             window.value()->size()));
        if (!renderer)
        {
            R4D_CRIT("Renderer creation failed: {}", renderer.error().ToString());
            return 1;
        }
        auto& gpu = *renderer.value();

        R4D_INFO("Controls: Esc quit, F1 toggle vsync");

        // Odd values catch assumptions about even dimensions.
        constexpr std::array<UVec2, 4> kResizeSizes = {
            UVec2{1280, 720}, UVec2{801, 457}, UVec2{1920, 1080}, UVec2{640, 360}};
        std::uint32_t resizesDone = 0;
        const std::uint64_t totalResizes =
            static_cast<std::uint64_t>(options.resizeCycles) * kResizeSizes.size();

        std::uint64_t frame = 0;
        double titleTimer = 0.0;
        int exitCode = 0;

        while (!window.value()->shouldClose())
        {
            window.value()->pumpEvents();

            UVec2 newSize{};
            if (window.value()->consumeResized(newSize))
            {
                if (auto status = gpu.resize(newSize); !status)
                {
                    R4D_CRIT("Resize failed: {}", status.error().ToString());
                    exitCode = 1;
                    break;
                }
            }

            const auto& input = window.value()->input();
            if (input.pressed(Key::Escape))
            {
                R4D_INFO("Escape pressed, shutting down");
                window.value()->requestClose();
            }
            if (input.pressed(Key::F1))
            {
                gpu.setVsync(!gpu.vsync());
                R4D_INFO("vsync {}", gpu.vsync() ? "on" : "off");
            }

            if (auto status = gpu.renderFrame(); !status)
            {
                R4D_CRIT("renderFrame failed: {}", status.error().ToString());
                exitCode = 1;
                break;
            }

            ++frame;

            if (resizesDone < totalResizes && frame % 45 == 0)
            {
                const UVec2 target = kResizeSizes[resizesDone % kResizeSizes.size()];
                R4D_INFO("Resize exercise {}/{}: requesting {}x{}", resizesDone + 1,
                         totalResizes, target.x, target.y);
                window.value()->setClientSize(target);
                ++resizesDone;
            }

            const auto& stats = gpu.stats();
            titleTimer += stats.cpuFrameMs;
            if (titleTimer > 500.0)
            {
                titleTimer = 0.0;
                const std::string gpuText =
                    stats.gpuTimingsValid
                        ? std::format("GPU {:.2f} ms", stats.gpuFrameAverageMs)
                        : std::string{"GPU --"};
                window.value()->setTitle(std::format(
                    "Radiance4D -- {} -- {}x{} -- CPU {:.2f} ms -- {} -- vsync {}",
                    ToString(gpu.backend()), stats.renderResolution.x,
                    stats.renderResolution.y, stats.cpuFrameAverageMs, gpuText,
                    gpu.vsync() ? "on" : "off"));
            }

            if (options.frameLimit != 0 && frame >= options.frameLimit)
            {
                R4D_INFO("Frame limit of {} reached", options.frameLimit);
                break;
            }
        }

        if (exitCode == 0 && !writeCaptureIfRequested(gpu, options))
        {
            exitCode = 1;
        }

        logFrameSummary(gpu, frame);
        exitCode |= reportValidation(gpu, options, frame);

        // The swapchain holds the HWND, so the renderer goes first.
        renderer.value().reset();
        window.value().reset();
        return exitCode;
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace r4d;

    Options options;
    if (!parseOptions(argc, argv, options))
    {
        printUsage();
        return 2;
    }
    if (options.help)
    {
        printUsage();
        return 0;
    }

    Logger::instance().init(static_cast<LogLevel>(options.logLevel), true,
                            executableDirectory() / "radiance4d.log");

    R4D_INFO("{} {} starting", kProjectName, kVersionString);
    R4D_INFO("Build: C++{}, {} bit", __cplusplus, sizeof(void*) * 8);

    return options.headless ? runHeadless(options) : runWindowed(options);
}
