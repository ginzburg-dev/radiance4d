// Time.cpp
#include <radiance4d/core/Time.hpp>

namespace r4d
{

    double FrameTimer::tick()
    {
        const double now = m_clock.elapsedSeconds();

        if (m_frameCount == 0)
        {
            // No previous frame to measure against.
            m_delta = 0.0;
        }
        else
        {
            m_delta = now - m_lastTime;
            m_history.add(m_delta);
        }

        m_lastTime = now;
        m_total = now;
        ++m_frameCount;
        return m_delta;
    }

} // namespace r4d
