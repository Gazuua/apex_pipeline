#include <apex/core/error_code.hpp>
#include <apex/shared/adapters/adapter_error.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;
using apex::core::ErrorCode;

TEST(AdapterError, DefaultIsAdapterError)
{
    AdapterError err;
    EXPECT_EQ(err.code, ErrorCode::AdapterError);
    EXPECT_EQ(err.native_error, 0u);
    EXPECT_TRUE(err.message.empty());
    EXPECT_FALSE(err.ok());
}

TEST(AdapterError, OkWhenCodeIsOk)
{
    AdapterError err{.code = ErrorCode::Ok, .message = {}};
    EXPECT_TRUE(err.ok());
}

TEST(AdapterError, ToStringBasic)
{
    AdapterError err{.native_error = 42, .message = "connection refused"};
    auto str = err.to_string();
    EXPECT_NE(str.find("42"), std::string::npos);
    EXPECT_NE(str.find("connection refused"), std::string::npos);
}

TEST(AdapterError, ToStringNoNativeError)
{
    AdapterError err{.message = "timeout"};
    auto str = err.to_string();
    EXPECT_EQ(str.find("native"), std::string::npos);
    EXPECT_NE(str.find("timeout"), std::string::npos);
}
