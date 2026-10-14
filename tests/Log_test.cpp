// Log_test.cpp
//
// The counters drive the validation exit code, so they are what matters here.

#include <radiance4d/core/Log.hpp>

#include <gtest/gtest.h>

namespace
{
    class LogCounters : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // Quiet; only the counters matter here.
            r4d::Logger::instance().init(r4d::LogLevel::Critical, false, {});
            r4d::Logger::instance().resetCounters();
        }

        void TearDown() override { r4d::Logger::instance().resetCounters(); }
    };
} // namespace

TEST_F(LogCounters, StartAtZero)
{
    EXPECT_EQ(r4d::Logger::instance().errorCount(), 0u);
    EXPECT_EQ(r4d::Logger::instance().messageCount(r4d::LogLevel::Warning), 0u);
}

TEST_F(LogCounters, CountErrorsAndCriticalsTogether)
{
    auto& logger = r4d::Logger::instance();
    logger.log(r4d::LogLevel::Error, "one");
    logger.log(r4d::LogLevel::Error, "two");
    logger.log(r4d::LogLevel::Critical, "three");

    EXPECT_EQ(logger.messageCount(r4d::LogLevel::Error), 2u);
    EXPECT_EQ(logger.messageCount(r4d::LogLevel::Critical), 1u);
    EXPECT_EQ(logger.errorCount(), 3u);
}

TEST_F(LogCounters, CountMessagesThatTheLevelFilterSuppresses)
{
    // Otherwise raising the log level would disarm the validation gate.
    auto& logger = r4d::Logger::instance();
    logger.setMinLevel(r4d::LogLevel::Critical);
    logger.log(r4d::LogLevel::Error, "suppressed from output but still counted");

    EXPECT_EQ(logger.errorCount(), 1u);
}

TEST_F(LogCounters, DoNotCountUnrelatedLevelsAsErrors)
{
    auto& logger = r4d::Logger::instance();
    logger.log(r4d::LogLevel::Warning, "warn");
    logger.log(r4d::LogLevel::Info, "info");
    logger.log(r4d::LogLevel::Trace, "trace");

    EXPECT_EQ(logger.errorCount(), 0u);
    EXPECT_EQ(logger.messageCount(r4d::LogLevel::Warning), 1u);
}

TEST_F(LogCounters, ResetClearsEverything)
{
    auto& logger = r4d::Logger::instance();
    logger.log(r4d::LogLevel::Error, "boom");
    ASSERT_EQ(logger.errorCount(), 1u);

    logger.resetCounters();
    EXPECT_EQ(logger.errorCount(), 0u);
}

TEST(LogLevel, HasStableNames)
{
    EXPECT_EQ(r4d::ToString(r4d::LogLevel::Trace), "TRACE");
    EXPECT_EQ(r4d::ToString(r4d::LogLevel::Error), "ERROR");
    EXPECT_EQ(r4d::ToString(r4d::LogLevel::Critical), "CRIT");
}
