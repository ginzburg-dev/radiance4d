// RollingAverage_test.cpp

#include <radiance4d/core/RollingAverage.hpp>

#include <gtest/gtest.h>

TEST(RollingAverage, StartsEmpty)
{
    r4d::RollingAverage<4> average;
    EXPECT_TRUE(average.empty());
    EXPECT_EQ(average.sampleCount(), 0u);
    EXPECT_DOUBLE_EQ(average.average(), 0.0);
    EXPECT_DOUBLE_EQ(average.latest(), 0.0);
}

TEST(RollingAverage, AveragesOnlyTheSamplesActuallyAdded)
{
    // With capacity 4 and one sample of 10 the mean is 10, not 10/4.
    r4d::RollingAverage<4> average;
    average.add(10.0);

    EXPECT_EQ(average.sampleCount(), 1u);
    EXPECT_DOUBLE_EQ(average.average(), 10.0);
}

TEST(RollingAverage, AveragesAPartiallyFilledWindow)
{
    r4d::RollingAverage<8> average;
    average.add(2.0);
    average.add(4.0);
    average.add(6.0);

    EXPECT_EQ(average.sampleCount(), 3u);
    EXPECT_DOUBLE_EQ(average.average(), 4.0);
}

TEST(RollingAverage, SaturatesAtCapacityAndDiscardsOldestSamples)
{
    r4d::RollingAverage<3> average;
    average.add(1.0);
    average.add(2.0);
    average.add(3.0);
    EXPECT_EQ(average.sampleCount(), 3u);
    EXPECT_DOUBLE_EQ(average.average(), 2.0);

    // Pushes out the 1.0.
    average.add(4.0);
    EXPECT_EQ(average.sampleCount(), 3u);
    EXPECT_DOUBLE_EQ(average.average(), 3.0);
}

TEST(RollingAverage, LatestTracksTheMostRecentSampleAcrossWrap)
{
    r4d::RollingAverage<2> average;
    average.add(5.0);
    EXPECT_DOUBLE_EQ(average.latest(), 5.0);
    average.add(7.0);
    EXPECT_DOUBLE_EQ(average.latest(), 7.0);
    average.add(9.0);
    EXPECT_DOUBLE_EQ(average.latest(), 9.0);
}

TEST(RollingAverage, ResetClearsEverything)
{
    r4d::RollingAverage<4> average;
    average.add(1.0);
    average.add(2.0);
    average.reset();

    EXPECT_TRUE(average.empty());
    EXPECT_DOUBLE_EQ(average.average(), 0.0);
}

TEST(RollingAverage, SingleSlotKeepsOnlyTheNewest)
{
    r4d::RollingAverage<1> average;
    average.add(1.0);
    average.add(99.0);

    EXPECT_EQ(average.sampleCount(), 1u);
    EXPECT_DOUBLE_EQ(average.average(), 99.0);
}
