// Time.hpp
#pragma once

#include <radiance4d/core/RollingAverage.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace r4d
{

    class Clock
    {
    public:
        using Duration = std::chrono::duration<double>;

        Clock() : m_start{std::chrono::steady_clock::now()} {}

        void reset() { m_start = std::chrono::steady_clock::now(); }

        double elapsedSeconds() const
        {
            return Duration{std::chrono::steady_clock::now() - m_start}.count();
        }

        double elapsedMilliseconds() const { return elapsedSeconds() * 1000.0; }

    private:
        std::chrono::steady_clock::time_point m_start;
    };

    class FrameTimer
    {
    public:
        static constexpr std::size_t kHistorySize = 120;

        double tick();

        double deltaSeconds() const { return m_delta; }
        double deltaMilliseconds() const { return m_delta * 1000.0; }
        double averageMilliseconds() const { return m_history.average() * 1000.0; }
        double totalSeconds() const { return m_total; }
        std::uint64_t frameCount() const { return m_frameCount; }

    private:
        Clock m_clock;
        double m_lastTime = 0.0;
        double m_delta = 0.0;
        double m_total = 0.0;
        std::uint64_t m_frameCount = 0;
        RollingAverage<kHistorySize> m_history;
    };

} // namespace r4d
