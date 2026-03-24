// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/scoped_logger.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <sstream>

using namespace apex::core;

namespace
{

class ScopedLoggerTest : public ::testing::Test
{
  protected:
    std::ostringstream oss_;

    void SetUp() override
    {
        spdlog::shutdown();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss_);
        auto apex_logger = std::make_shared<spdlog::logger>("apex", sink);
        apex_logger->set_level(spdlog::level::trace);
        apex_logger->set_pattern("%v"); // message only — no timestamp/level prefix
        spdlog::register_logger(apex_logger);

        auto app_logger = std::make_shared<spdlog::logger>("app", sink);
        app_logger->set_level(spdlog::level::trace);
        app_logger->set_pattern("%v");
        spdlog::register_logger(app_logger);
    }

    void TearDown() override
    {
        spdlog::shutdown();
    }
};

} // anonymous namespace

TEST_F(ScopedLoggerTest, BasicInfo)
{
    ScopedLogger logger("CoreEngine", 0);
    logger.info("tick completed");
    auto output = oss_.str();
    EXPECT_NE(output.find("[core=0]"), std::string::npos);
    EXPECT_NE(output.find("[CoreEngine]"), std::string::npos);
    EXPECT_NE(output.find("tick completed"), std::string::npos);
    // source_location should capture this test file
    EXPECT_NE(output.find("test_scoped_logger.cpp"), std::string::npos);
}

TEST_F(ScopedLoggerTest, BasicInfoWithFormatArgs)
{
    ScopedLogger logger("CoreEngine", 3);
    logger.info("processed {} items in {}ms", 42, 17);
    auto output = oss_.str();
    EXPECT_NE(output.find("[core=3]"), std::string::npos);
    EXPECT_NE(output.find("processed 42 items in 17ms"), std::string::npos);
}

TEST_F(ScopedLoggerTest, WithSession)
{
    ScopedLogger logger("AuthService", 2, "app");
    logger.debug(make_session_id(7), "PG user lookup");
    auto output = oss_.str();
    EXPECT_NE(output.find("[core=2]"), std::string::npos);
    EXPECT_NE(output.find("[AuthService]"), std::string::npos);
    EXPECT_NE(output.find("[sess=7]"), std::string::npos);
    EXPECT_NE(output.find("PG user lookup"), std::string::npos);
}

TEST_F(ScopedLoggerTest, WithSessionAndMsgId)
{
    ScopedLogger logger("AuthService", 2, "app");
    logger.debug(make_session_id(7), 0x0012u, "token issued");
    auto output = oss_.str();
    EXPECT_NE(output.find("[sess=7]"), std::string::npos);
    EXPECT_NE(output.find("[msg=0x0012]"), std::string::npos);
    EXPECT_NE(output.find("token issued"), std::string::npos);
}

TEST_F(ScopedLoggerTest, WithTrace)
{
    ScopedLogger logger("GatewayService", 1, "app");
    auto traced = logger.with_trace(0xa1b2c3d4);
    traced.info("routing complete");
    auto output = oss_.str();
    EXPECT_NE(output.find("[trace=a1b2c3d4]"), std::string::npos);
    EXPECT_NE(output.find("routing complete"), std::string::npos);
}

TEST_F(ScopedLoggerTest, NoCoreContext)
{
    ScopedLogger logger("RedisConnection", ScopedLogger::NO_CORE);
    logger.warn("reconnecting...");
    auto output = oss_.str();
    // [core=N] tag should NOT appear
    EXPECT_EQ(output.find("[core="), std::string::npos);
    EXPECT_NE(output.find("[RedisConnection]"), std::string::npos);
    EXPECT_NE(output.find("reconnecting..."), std::string::npos);
}

TEST_F(ScopedLoggerTest, LevelFiltering)
{
    auto apex_logger = spdlog::get("apex");
    apex_logger->set_level(spdlog::level::info);

    ScopedLogger logger("Test", 0);
    EXPECT_FALSE(logger.should_log(spdlog::level::debug));
    EXPECT_TRUE(logger.should_log(spdlog::level::info));

    logger.debug("should not appear");
    logger.info("should appear");

    auto output = oss_.str();
    EXPECT_EQ(output.find("should not appear"), std::string::npos);
    EXPECT_NE(output.find("should appear"), std::string::npos);
}

TEST_F(ScopedLoggerTest, ApexVsAppLogger)
{
    // Recreate with separate sinks to verify logger routing
    spdlog::shutdown();
    std::ostringstream apex_oss, app_oss;

    auto apex_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(apex_oss);
    auto apex = std::make_shared<spdlog::logger>("apex", apex_sink);
    apex->set_level(spdlog::level::trace);
    apex->set_pattern("%v");
    spdlog::register_logger(apex);

    auto app_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(app_oss);
    auto app = std::make_shared<spdlog::logger>("app", app_sink);
    app->set_level(spdlog::level::trace);
    app->set_pattern("%v");
    spdlog::register_logger(app);

    ScopedLogger apex_logger("CoreEngine", 0);        // defaults to "apex"
    ScopedLogger app_logger("AuthService", 0, "app"); // explicitly "app"

    apex_logger.info("framework message");
    app_logger.info("service message");

    EXPECT_NE(apex_oss.str().find("framework message"), std::string::npos);
    EXPECT_EQ(apex_oss.str().find("service message"), std::string::npos);

    EXPECT_NE(app_oss.str().find("service message"), std::string::npos);
    EXPECT_EQ(app_oss.str().find("framework message"), std::string::npos);
}

TEST_F(ScopedLoggerTest, NullLoggerDoesNotCrash)
{
    // Logger for a name that doesn't exist → null → should silently no-op
    ScopedLogger logger("Test", 0, "nonexistent");
    EXPECT_FALSE(logger.should_log(spdlog::level::info));
    EXPECT_NO_THROW(logger.info("this should not crash"));
    EXPECT_TRUE(oss_.str().empty());
}

TEST_F(ScopedLoggerTest, AllSixLevels)
{
    ScopedLogger logger("Test", 0);
    logger.trace("trace msg");
    logger.debug("debug msg");
    logger.info("info msg");
    logger.warn("warn msg");
    logger.error("error msg");
    logger.critical("critical msg");

    auto output = oss_.str();
    EXPECT_NE(output.find("trace msg"), std::string::npos);
    EXPECT_NE(output.find("debug msg"), std::string::npos);
    EXPECT_NE(output.find("info msg"), std::string::npos);
    EXPECT_NE(output.find("warn msg"), std::string::npos);
    EXPECT_NE(output.find("error msg"), std::string::npos);
    EXPECT_NE(output.find("critical msg"), std::string::npos);
}
