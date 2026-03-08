#include <apex/core/logging.hpp>

#include <spdlog/spdlog.h>
#include <gtest/gtest.h>

#include <filesystem>

using namespace apex::core;

namespace {

class LoggingTest : public ::testing::Test {
protected:
    void TearDown() override {
        shutdown_logging();
        // 테스트 간 로거 격리
    }
};

} // anonymous namespace

TEST_F(LoggingTest, InitCreatesApexLogger) {
    LogConfig cfg;
    init_logging(cfg);
    auto logger = spdlog::get("apex");
    ASSERT_NE(logger, nullptr);
}

TEST_F(LoggingTest, InitCreatesAppLogger) {
    LogConfig cfg;
    init_logging(cfg);
    auto logger = spdlog::get("app");
    ASSERT_NE(logger, nullptr);
}

TEST_F(LoggingTest, FrameworkLevelIndependent) {
    LogConfig cfg;
    cfg.level = "info";
    cfg.framework_level = "debug";
    init_logging(cfg);

    auto apex = spdlog::get("apex");
    auto app = spdlog::get("app");
    EXPECT_EQ(apex->level(), spdlog::level::debug);
    EXPECT_EQ(app->level(), spdlog::level::info);
}

TEST_F(LoggingTest, ConsoleOnlyByDefault) {
    LogConfig cfg;  // console.enabled=true, file.enabled=false
    init_logging(cfg);
    auto logger = spdlog::get("apex");
    // 기본 설정: ConsoleSink 1개만
    EXPECT_EQ(logger->sinks().size(), 1);
}

TEST_F(LoggingTest, FileEnabled) {
    auto tmp = std::filesystem::temp_directory_path() / "apex_log_test";
    std::filesystem::create_directories(tmp);
    auto log_path = (tmp / "test.log").string();

    {
        LogConfig cfg;
        cfg.file.enabled = true;
        cfg.file.path = log_path;
        init_logging(cfg);

        auto logger = spdlog::get("apex");
        EXPECT_EQ(logger->sinks().size(), 2);  // Console + File

        logger->info("test message");
        logger->flush();

        EXPECT_TRUE(std::filesystem::exists(log_path));
    }

    // Drop all loggers to release file handles before cleanup
    spdlog::drop_all();
    std::filesystem::remove_all(tmp);
}

TEST_F(LoggingTest, ShutdownCleansUp) {
    LogConfig cfg;
    init_logging(cfg);
    shutdown_logging();
    EXPECT_EQ(spdlog::get("apex"), nullptr);
    EXPECT_EQ(spdlog::get("app"), nullptr);
}

TEST_F(LoggingTest, DoubleInitIsSafe) {
    LogConfig cfg;
    init_logging(cfg);
    // Second init should not crash or corrupt state
    EXPECT_NO_THROW(init_logging(cfg));
    EXPECT_NE(spdlog::get("apex"), nullptr);
    EXPECT_NE(spdlog::get("app"), nullptr);
}

TEST_F(LoggingTest, LoggingAfterShutdownDoesNotCrash) {
    LogConfig cfg;
    init_logging(cfg);
    shutdown_logging();
    // Loggers should be gone
    EXPECT_EQ(spdlog::get("apex"), nullptr);
    // Null-safe: attempting to log via a null logger pointer must not crash
    auto logger = spdlog::get("apex");
    if (logger) {
        logger->info("should not reach here");
    }
    // If we reach here, no crash occurred
}
