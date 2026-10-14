// Log.hpp
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#ifndef R4D_MIN_LOG_LEVEL
#define R4D_MIN_LOG_LEVEL 0
#endif

namespace r4d
{

    enum class LogLevel
    {
        Trace    = 0,
        Debug    = 1,
        Info     = 2,
        Warning  = 3,
        Error    = 4,
        Critical = 5,
    };

    constexpr std::string_view ToString(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::Trace:    return "TRACE";
            case LogLevel::Debug:    return "DEBUG";
            case LogLevel::Info:     return "INFO";
            case LogLevel::Warning:  return "WARN";
            case LogLevel::Error:    return "ERROR";
            case LogLevel::Critical: return "CRIT";
        }
        return "?????";
    }

    class Logger
    {
    public:
        static Logger& instance();

        void init(LogLevel minLevel, bool toConsole, const std::filesystem::path& logFile = {});

        void log(LogLevel level, std::string_view message);

        std::uint64_t messageCount(LogLevel level) const;
        std::uint64_t errorCount() const;
        void resetCounters();

        void setMinLevel(LogLevel level);
        LogLevel minLevel() const;

    private:
        Logger() = default;
        ~Logger();
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        static std::string timestamp();

        mutable std::mutex m_mutex;
        LogLevel m_minLevel = LogLevel::Trace;
        bool m_toConsole = true;
        std::ofstream m_file;
        std::array<std::uint64_t, 6> m_counts{};
    };

} // namespace r4d

#define R4D_LOG_AT(level, levelValue, ...)                                      \
    do {                                                                        \
        if constexpr ((levelValue) >= (R4D_MIN_LOG_LEVEL)) {                    \
            ::r4d::Logger::instance().log((level), ::std::format(__VA_ARGS__)); \
        }                                                                       \
    } while (false)

#define R4D_TRACE(...) R4D_LOG_AT(::r4d::LogLevel::Trace,    0, __VA_ARGS__)
#define R4D_DEBUG(...) R4D_LOG_AT(::r4d::LogLevel::Debug,    1, __VA_ARGS__)
#define R4D_INFO(...)  R4D_LOG_AT(::r4d::LogLevel::Info,     2, __VA_ARGS__)
#define R4D_WARN(...)  R4D_LOG_AT(::r4d::LogLevel::Warning,  3, __VA_ARGS__)
#define R4D_ERROR(...) R4D_LOG_AT(::r4d::LogLevel::Error,    4, __VA_ARGS__)
#define R4D_CRIT(...)  R4D_LOG_AT(::r4d::LogLevel::Critical, 5, __VA_ARGS__)
