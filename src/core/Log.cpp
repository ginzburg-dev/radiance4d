// Log.cpp
#include <radiance4d/core/Log.hpp>

#include <chrono>
#include <ctime>
#include <iostream>

namespace r4d
{

    Logger& Logger::instance()
    {
        static Logger inst;
        return inst;
    }

    Logger::~Logger()
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }
    }

    void Logger::init(LogLevel minLevel, bool toConsole,
                      const std::filesystem::path& logFile)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_minLevel = minLevel;
        m_toConsole = toConsole;

        if (m_file.is_open())
        {
            m_file.close();
        }
        if (!logFile.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(logFile.parent_path(), ec);
            m_file.open(logFile, std::ios::out | std::ios::trunc);
        }
    }

    std::string Logger::timestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return std::format("{}.{:03}", buf, ms.count());
    }

    void Logger::log(LogLevel level, std::string_view message)
    {
        std::lock_guard<std::mutex> lock{m_mutex};

        // Count before filtering: a message we chose not to print still happened.
        ++m_counts[static_cast<std::size_t>(level)];

        if (level < m_minLevel)
        {
            return;
        }

        const std::string line =
            std::format("[{}] [{:<5}] {}", timestamp(), ToString(level), message);

        if (m_toConsole)
        {
            std::ostream& out = (level >= LogLevel::Error) ? std::cerr : std::cout;
            out << line << '\n';
            if (level >= LogLevel::Warning)
            {
                out.flush();
            }
        }
        if (m_file.is_open())
        {
            m_file << line << '\n';
            if (level >= LogLevel::Warning)
            {
                m_file.flush();
            }
        }
    }

    std::uint64_t Logger::messageCount(LogLevel level) const
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_counts[static_cast<std::size_t>(level)];
    }

    std::uint64_t Logger::errorCount() const
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_counts[static_cast<std::size_t>(LogLevel::Error)] +
               m_counts[static_cast<std::size_t>(LogLevel::Critical)];
    }

    void Logger::resetCounters()
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_counts.fill(0);
    }

    void Logger::setMinLevel(LogLevel level)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_minLevel = level;
    }

    LogLevel Logger::minLevel() const
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_minLevel;
    }

} // namespace r4d
