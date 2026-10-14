// Renderer.hpp
#pragma once

// The only polymorphic boundary in the graphics module. Backend-specific types
// stay behind it; nothing here may pull in d3d12.h or dxgi.h.

#include <radiance4d/core/Math.hpp>
#include <radiance4d/core/Result.hpp>
#include <radiance4d/graphics/AdapterInfo.hpp>
#include <radiance4d/graphics/BackendType.hpp>
#include <radiance4d/graphics/FrameCapture.hpp>
#include <radiance4d/graphics/GpuCapabilities.hpp>
#include <radiance4d/graphics/GpuStats.hpp>

#include <memory>
#include <string>

namespace r4d
{

    struct RendererDesc
    {
        // Null means headless: offscreen target, nothing presented.
        void* windowHandle = nullptr;

        UVec2 initialSize{1600, 900};

        // Empty picks the fastest hardware adapter. Also accepts "warp", an
        // index, or part of an adapter name.
        std::string adapterSelector;

        bool enableDebugLayer = false;
        bool enableGpuValidation = false;
        bool vsync = true;

        // Seconds per frame instead of the wall clock. Required for a
        // reproducible capture.
        double fixedTimeStep = 0.0;

        // Empty means <executable dir>/shaders.
        std::string shaderDirectory;
    };

    struct CameraParams
    {
        Mat4 view{1.0f};
        Mat4 projection{1.0f};
        Vec3 position{0.0f, 0.0f, 0.0f};
    };

    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        virtual void setCamera(const CameraParams& camera) = 0;

        virtual Status renderFrame() = 0;

        // Safe with an unchanged size, and with a zero size while minimised.
        virtual Status resize(UVec2 newSize) = 0;

        virtual void waitForGpuIdle() = 0;

        // Synchronous; for tests and tooling, not for a per-frame path.
        virtual Result<FrameCapture> captureFrame() = 0;

        virtual bool headless() const = 0;

        virtual BackendType backend() const = 0;
        virtual const AdapterInfo& adapterInfo() const = 0;
        virtual const GpuCapabilities& capabilities() const = 0;
        virtual const GpuStats& stats() const = 0;

        virtual void setVsync(bool enabled) = 0;
        virtual bool vsync() const = 0;

        virtual std::uint64_t validationErrorCount() const = 0;

    protected:
        Renderer() = default;
    };

    Result<std::unique_ptr<Renderer>> createRenderer(BackendType backend,
                                                     const RendererDesc& desc);

} // namespace r4d
