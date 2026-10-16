// Result_test.cpp
#include <radiance4d/core/Result.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{
    r4d::Result<int> succeeds()
    {
        return 42;
    }

    r4d::Result<int> fails()
    {
        return r4d::makeError(r4d::ErrorCode::NotFound, "nothing here");
    }

    r4d::Status propagates()
    {
        R4D_TRY(fails());
        return r4d::Status::Ok();
    }
} // namespace

TEST(Result, CarriesAValueOnSuccess)
{
    auto result = succeeds();
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
}

TEST(Result, CarriesAnErrorOnFailure)
{
    auto result = fails();
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.error().code, r4d::ErrorCode::NotFound);
    EXPECT_EQ(result.error().message, "nothing here");
    EXPECT_EQ(result.error().hresult, 0);
}

TEST(Result, TryMacroPropagatesTheError)
{
    auto status = propagates();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, r4d::ErrorCode::NotFound);
}

TEST(Status, DefaultsToSuccess)
{
    r4d::Status status;
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(r4d::Status::Ok().ok());
}

TEST(Error, FormatsAnHresultWhenPresent)
{
    const auto error = r4d::makeError(r4d::ErrorCode::ApiCallFailed, "CreateDevice",
                                      static_cast<long>(0x887A0004));
    const std::string text = error.ToString();
    EXPECT_NE(text.find("ApiCallFailed"), std::string::npos);
    EXPECT_NE(text.find("CreateDevice"), std::string::npos);
    EXPECT_NE(text.find("887A0004"), std::string::npos);
}

TEST(Error, OmitsTheHresultWhenAbsent)
{
    const auto error = r4d::makeError(r4d::ErrorCode::IoFailure, "bad file");
    EXPECT_EQ(error.ToString().find("hr="), std::string::npos);
}
