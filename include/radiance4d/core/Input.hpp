// Input.hpp
#pragma once

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
        Count
    };

    class InputState
    {
    public:
        bool down(Key key) const { return m_down[index(key)]; }

        // True only on the frame the key went down.
        bool pressed(Key key) const { return m_pressed[index(key)]; }

        void newFrame() { m_pressed.fill(false); }

        void setDown(Key key, bool isDown)
        {
            const std::size_t i = index(key);
            if (isDown && !m_down[i]) m_pressed[i] = true;
            m_down[i] = isDown;
        }

    private:
        static constexpr std::size_t index(Key key) { return static_cast<std::size_t>(key); }
        static constexpr std::size_t kCount = static_cast<std::size_t>(Key::Count);

        std::array<bool, kCount> m_down{};
        std::array<bool, kCount> m_pressed{};
    };

} // namespace r4d
