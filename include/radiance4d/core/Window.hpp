// Window.hpp
#pragma once

#include <radiance4d/core/Input.hpp>
#include <radiance4d/core/Math.hpp>
#include <radiance4d/core/Result.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace r4d
{

    struct WindowDesc
    {
        std::uint32_t width = 1600;
        std::uint32_t height = 900;
        std::string title = "Radiance4D";
    };

    class Window
    {
    public:
        static Result<std::unique_ptr<Window>> create(const WindowDesc& desc);

        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void pumpEvents();

        bool shouldClose() const { return m_shouldClose; }
        void requestClose() { m_shouldClose = true; }

        // HWND, kept opaque so windows.h stays out of the public headers.
        void* nativeHandle() const { return m_hwnd; }

        // Client area in pixels; (0,0) while minimised.
        UVec2 size() const { return m_size; }
        bool isMinimised() const { return m_size.x == 0 || m_size.y == 0; }

        bool consumeResized(UVec2& outNewSize);

        const InputState& input() const { return m_input; }

        // Return true to consume the message before Window handles it.
        using MessageHook =
            std::function<bool(void*, unsigned, unsigned long long, long long)>;
        void setMessageHook(MessageHook hook) { m_messageHook = std::move(hook); }

        void setTitle(const std::string& title);
        void setClientSize(UVec2 size);

    private:
        Window() = default;

        long long handleMessage(unsigned msg, unsigned long long wparam, long long lparam);
        friend struct WindowProcAccess;

        void* m_hwnd = nullptr;
        UVec2 m_size{0, 0};
        bool m_shouldClose = false;
        bool m_resized = false;
        InputState m_input;
        MessageHook m_messageHook;
    };

} // namespace r4d
