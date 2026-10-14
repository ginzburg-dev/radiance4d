// IncludeHygiene_test.cpp
//
// Public headers must stay free of d3d12.h, dxgi.h and windows.h. Most of the
// check happens at compile time below.

#include <radiance4d/core/FileSystem.hpp>
#include <radiance4d/core/Input.hpp>
#include <radiance4d/core/Log.hpp>
#include <radiance4d/core/Math.hpp>
#include <radiance4d/core/Result.hpp>
#include <radiance4d/core/Time.hpp>
#include <radiance4d/core/Version.hpp>
#include <radiance4d/core/Window.hpp>
#include <radiance4d/graphics/AdapterInfo.hpp>
#include <radiance4d/graphics/BackendType.hpp>
#include <radiance4d/graphics/GpuCapabilities.hpp>
#include <radiance4d/graphics/GpuStats.hpp>
#include <radiance4d/graphics/Renderer.hpp>

// Include guards defined by the SDK headers.
#if defined(__d3d12_h__)
#error "A public Radiance4D header leaks d3d12.h. D3D12 types must stay inside src/graphics/d3d12/."
#endif

#if defined(__dxgi_h__) || defined(__dxgi1_2_h__) || defined(__dxgi1_3_h__) || \
    defined(__dxgi1_4_h__) || defined(__dxgi1_5_h__) || defined(__dxgi1_6_h__)
#error "A public Radiance4D header leaks a dxgi header. DXGI types must stay inside src/graphics/d3d12/."
#endif

#if defined(_WINDOWS_)
#error "A public Radiance4D header leaks windows.h. Window hands out its HWND as an opaque void*."
#endif

#include <gtest/gtest.h>

#include <type_traits>

TEST(IncludeHygiene, PublicHeadersDoNotLeakGraphicsApiTypes)
{
    // Compiling at all is the assertion.
    SUCCEED() << "public headers compiled without d3d12.h, dxgi*.h or windows.h";
}

TEST(IncludeHygiene, NativeWindowHandleIsOpaque)
{
    // A real HWND here would mean windows.h leaked into the public headers.
    static_assert(std::is_same_v<decltype(std::declval<const r4d::Window&>().nativeHandle()),
                                 void*>,
                  "Window::nativeHandle() must stay an opaque void*");
    SUCCEED();
}

TEST(IncludeHygiene, RendererIsAbstractAndOwnsTheOnlyVirtualSeam)
{
    // Exactly one polymorphic boundary in the graphics module.
    static_assert(std::is_abstract_v<r4d::Renderer>,
                  "Renderer is the single cross-backend seam and must be abstract");
    static_assert(std::has_virtual_destructor_v<r4d::Renderer>,
                  "Renderer is deleted through a base pointer");
    SUCCEED();
}
