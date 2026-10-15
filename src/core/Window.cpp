// Window.cpp
#include <radiance4d/core/Window.hpp>

#include <radiance4d/core/Log.hpp>

#include <windows.h>

#include <format>

namespace r4d
{

    // Lets the WNDPROC reach Window internals without Win32 types in the header.
    struct WindowProcAccess
    {
        static void bind(Window* window, void* hwnd) { window->m_hwnd = hwnd; }

        static LRESULT dispatch(Window* window, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            return static_cast<LRESULT>(
                window->handleMessage(msg,
                                      static_cast<unsigned long long>(wparam),
                                      static_cast<long long>(lparam)));
        }
    };

} // namespace r4d

namespace
{
    constexpr const wchar_t* kWindowClassName = L"Radiance4DWindow";

    std::wstring utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int needed = ::MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              wide.data(), needed);
        return wide;
    }

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* self = static_cast<r4d::Window*>(create->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr)
            {
                r4d::WindowProcAccess::bind(self, hwnd);
            }
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        auto* self =
            reinterpret_cast<r4d::Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr)
        {
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        return r4d::WindowProcAccess::dispatch(self, msg, wparam, lparam);
    }

    bool registerWindowClass()
    {
        static const bool registered = []() {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
            wc.lpfnWndProc = &windowProc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kWindowClassName;
            // No background brush: erasing the client area flickers on resize.
            wc.hbrBackground = nullptr;
            return ::RegisterClassExW(&wc) != 0;
        }();
        return registered;
    }

    bool mapVirtualKey(WPARAM vk, r4d::Key& outKey)
    {
        switch (vk)
        {
            case VK_ESCAPE:  outKey = r4d::Key::Escape;  return true;
            case VK_F1:      outKey = r4d::Key::F1;      return true;
            case VK_F2:      outKey = r4d::Key::F2;      return true;
            case VK_F3:      outKey = r4d::Key::F3;      return true;
            case VK_F4:      outKey = r4d::Key::F4;      return true;
            case 'W':        outKey = r4d::Key::W;       return true;
            case 'A':        outKey = r4d::Key::A;       return true;
            case 'S':        outKey = r4d::Key::S;       return true;
            case 'D':        outKey = r4d::Key::D;       return true;
            case 'Q':        outKey = r4d::Key::Q;       return true;
            case 'E':        outKey = r4d::Key::E;       return true;
            case VK_SPACE:   outKey = r4d::Key::Space;   return true;
            case VK_SHIFT:   outKey = r4d::Key::Shift;   return true;
            case VK_CONTROL: outKey = r4d::Key::Control; return true;
            default:         return false;
        }
    }
} // namespace

namespace r4d
{

    Result<std::unique_ptr<Window>> Window::create(const WindowDesc& desc)
    {
        // Must precede the first window or the client size gets DPI-scaled.
        static const bool dpiConfigured =
            ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        if (!dpiConfigured)
        {
            R4D_WARN("Window: per-monitor DPI awareness v2 was refused; sizes may be scaled");
        }

        if (!registerWindowClass())
        {
            return makeError(ErrorCode::ApiCallFailed, "RegisterClassExW failed",
                             static_cast<long>(::GetLastError()));
        }

        // Size for the requested client area.
        RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        constexpr DWORD style = WS_OVERLAPPEDWINDOW;
        ::AdjustWindowRect(&rect, style, FALSE);

        auto window = std::unique_ptr<Window>{new Window{}};
        const std::wstring title = utf8ToWide(desc.title);

        const HWND hwnd = ::CreateWindowExW(
            0, kWindowClassName, title.c_str(), style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, ::GetModuleHandleW(nullptr), window.get());

        if (hwnd == nullptr)
        {
            return makeError(ErrorCode::ApiCallFailed, "CreateWindowExW failed",
                             static_cast<long>(::GetLastError()));
        }

        window->m_hwnd = hwnd;

        RECT client{};
        ::GetClientRect(hwnd, &client);
        window->m_size = UVec2{static_cast<std::uint32_t>(client.right - client.left),
                               static_cast<std::uint32_t>(client.bottom - client.top)};

        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);

        R4D_INFO("Window: created {}x{} client area", window->m_size.x, window->m_size.y);
        return window;
    }

    Window::~Window()
    {
        if (m_hwnd != nullptr)
        {
            const HWND hwnd = static_cast<HWND>(m_hwnd);
            // Messages can still arrive during DestroyWindow.
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            m_hwnd = nullptr;
            ::DestroyWindow(hwnd);
        }
    }

    void Window::pumpEvents()
    {
        m_input.newFrame();

        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_shouldClose = true;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    bool Window::consumeResized(UVec2& outNewSize)
    {
        if (!m_resized)
        {
            return false;
        }
        m_resized = false;
        outNewSize = m_size;
        return true;
    }

    void Window::setTitle(const std::string& title)
    {
        if (m_hwnd != nullptr)
        {
            ::SetWindowTextW(static_cast<HWND>(m_hwnd), utf8ToWide(title).c_str());
        }
    }

    void Window::setClientSize(UVec2 size)
    {
        if (m_hwnd == nullptr || size.x == 0 || size.y == 0)
        {
            return;
        }
        RECT rect{0, 0, static_cast<LONG>(size.x), static_cast<LONG>(size.y)};
        const HWND hwnd = static_cast<HWND>(m_hwnd);
        const LONG style = ::GetWindowLongW(hwnd, GWL_STYLE);
        ::AdjustWindowRect(&rect, static_cast<DWORD>(style), FALSE);
        ::SetWindowPos(hwnd, nullptr, 0, 0,
                       rect.right - rect.left, rect.bottom - rect.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    long long Window::handleMessage(unsigned msg, unsigned long long wparam, long long lparam)
    {
        const HWND hwnd = static_cast<HWND>(m_hwnd);

        if (m_messageHook && m_messageHook(m_hwnd, msg, wparam, lparam))
        {
            return 0;
        }

        switch (msg)
        {
            case WM_SIZE:
            {
                UVec2 newSize{0, 0};
                if (wparam != SIZE_MINIMIZED)
                {
                    newSize = UVec2{static_cast<std::uint32_t>(LOWORD(lparam)),
                                    static_cast<std::uint32_t>(HIWORD(lparam))};
                }
                if (newSize != m_size)
                {
                    m_size = newSize;
                    m_resized = true;
                }
                return 0;
            }

            case WM_CLOSE:
                // The renderer must drain in-flight frames first.
                m_shouldClose = true;
                return 0;

            case WM_DESTROY:
                m_shouldClose = true;
                ::PostQuitMessage(0);
                return 0;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            {
                Key key{};
                if (mapVirtualKey(static_cast<WPARAM>(wparam), key))
                {
                    m_input.setDown(key, true);
                    return 0;
                }
                break;
            }

            case WM_KEYUP:
            case WM_SYSKEYUP:
            {
                Key key{};
                if (mapVirtualKey(static_cast<WPARAM>(wparam), key))
                {
                    m_input.setDown(key, false);
                    return 0;
                }
                break;
            }

            case WM_MOUSEMOVE:
            {
                const int x = static_cast<short>(LOWORD(lparam));
                const int y = static_cast<short>(HIWORD(lparam));
                m_input.setMousePosition(Vec2{static_cast<float>(x), static_cast<float>(y)});
                return 0;
            }

            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            {
                // Keep receiving movement while a drag leaves the client area.
                ::SetCapture(hwnd);
                const MouseButton button = (msg == WM_LBUTTONDOWN)   ? MouseButton::Left
                                           : (msg == WM_RBUTTONDOWN) ? MouseButton::Right
                                                                     : MouseButton::Middle;
                m_input.setMouseDown(button, true);
                return 0;
            }

            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            {
                const MouseButton button = (msg == WM_LBUTTONUP)   ? MouseButton::Left
                                           : (msg == WM_RBUTTONUP) ? MouseButton::Right
                                                                   : MouseButton::Middle;
                m_input.setMouseDown(button, false);
                if (!m_input.mouseDown(MouseButton::Left) &&
                    !m_input.mouseDown(MouseButton::Right) &&
                    !m_input.mouseDown(MouseButton::Middle))
                {
                    ::ReleaseCapture();
                }
                return 0;
            }

            case WM_MOUSEWHEEL:
                m_input.addWheelDelta(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                                      static_cast<float>(WHEEL_DELTA));
                return 0;

            case WM_KILLFOCUS:
                // Otherwise keys held when focus was lost stay down forever.
                m_input.releaseAll();
                return 0;

            // Stop the beep on Alt and F10.
            case WM_SYSCHAR:
                return 0;

            default:
                break;
        }

        return ::DefWindowProcW(hwnd, static_cast<UINT>(msg),
                                static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    }

} // namespace r4d
