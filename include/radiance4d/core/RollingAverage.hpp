// RollingAverage.hpp
#pragma once

#include <array>
#include <cstddef>

namespace r4d
{

    // Averages only the samples actually added, so a partially filled window is
    // not diluted by its initial zeros.
    template<std::size_t Capacity>
    class RollingAverage
    {
        static_assert(Capacity > 0);

    public:
        static constexpr std::size_t capacity() { return Capacity; }

        void add(double sample)
        {
            m_samples[m_write] = sample;
            m_write = (m_write + 1) % Capacity;
            if (m_count < Capacity)
            {
                ++m_count;
            }
        }

        double average() const
        {
            if (m_count == 0)
            {
                return 0.0;
            }
            double sum = 0.0;
            for (std::size_t i = 0; i < m_count; ++i)
            {
                sum += m_samples[i];
            }
            return sum / static_cast<double>(m_count);
        }

        double latest() const
        {
            return m_count == 0 ? 0.0 : m_samples[(m_write + Capacity - 1) % Capacity];
        }

        std::size_t sampleCount() const { return m_count; }
        bool empty() const { return m_count == 0; }

        void reset()
        {
            m_samples.fill(0.0);
            m_write = 0;
            m_count = 0;
        }

    private:
        std::array<double, Capacity> m_samples{};
        std::size_t m_write = 0;
        std::size_t m_count = 0;
    };

} // namespace r4d
