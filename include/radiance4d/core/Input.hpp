// Input.hpp
#pragma once

#include <radiance4d/core/Math.hpp>

#include <array>
#include <cstddef>

namespace r4d
{

    enum class Key : std::size_t
    {
        Escape = 0,
        F1,
        F2,
        F3,
        F4,
        W,
        A,
        S,
        D,
        Q,
        E,
        Space,
        Shift,
        Control,
        Count
    };

    enum class MouseButton : std::size_t
    {
        Left = 0,
        Right,
        Middle,
        Count
    };

    class InputState
    {
    public:
        bool down(Key key) const { return m_down[index(key)]; }

        // True only on the frame the key went down.
        bool pressed(Key key) const { return m_pressed[index(key)]; }

        bool mouseDown(MouseButton button) const { return m_mouseDown[index(button)]; }

        Vec2 mousePosition() const { return m_mousePosition; }
        Vec2 mouseDelta() const { return m_mouseDelta; }
        float wheelDelta() const { return m_wheelDelta; }

        void newFrame()
        {
            m_pressed.fill(false);
            m_mouseDelta = Vec2{0.0f, 0.0f};
            m_wheelDelta = 0.0f;
        }

        void setDown(Key key, bool isDown)
        {
            const std::size_t i = index(key);
            if (isDown && !m_down[i])
                m_pressed[i] = true;
            m_down[i] = isDown;
        }

        void setMouseDown(MouseButton button, bool isDown)
        {
            m_mouseDown[index(button)] = isDown;
        }

        void setMousePosition(Vec2 position)
        {
            if (m_hasMousePosition)
            {
                m_mouseDelta += position - m_mousePosition;
            }
            m_mousePosition = position;
            m_hasMousePosition = true;
        }

        void addWheelDelta(float delta) { m_wheelDelta += delta; }

        // Called when the window loses focus, so held keys do not stick.
        void releaseAll()
        {
            m_down.fill(false);
            m_mouseDown.fill(false);
            m_hasMousePosition = false;
        }

    private:
        static constexpr std::size_t index(Key key)
        {
            return static_cast<std::size_t>(key);
        }
        static constexpr std::size_t index(MouseButton button)
        {
            return static_cast<std::size_t>(button);
        }
        static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
        static constexpr std::size_t kButtonCount =
            static_cast<std::size_t>(MouseButton::Count);

        std::array<bool, kKeyCount> m_down{};
        std::array<bool, kKeyCount> m_pressed{};
        std::array<bool, kButtonCount> m_mouseDown{};

        Vec2 m_mousePosition{0.0f, 0.0f};
        Vec2 m_mouseDelta{0.0f, 0.0f};
        float m_wheelDelta = 0.0f;
        bool m_hasMousePosition = false;
    };

} // namespace r4d
