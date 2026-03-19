#include <apex/core/logging.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <sstream>

using namespace apex::core;

namespace
{

class LoggingTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        shutdown_logging();
        // 테스트 간 로거 격리
    }
};

} // anonymous namespace

TEST_F(LoggingTest, InitCreatesApexLogger)
{
    LogConfig cfg;
    init_logging(cfg);
    auto logger = spdlog::get("apex");
    ASSERT_NE(logger, nullptr);
}

TEST_F(LoggingTest, InitCreatesAppLogger)
{
    LogConfig cfg;
    init_logging(cfg);
    auto logger = spdlog::get("app");
    ASSERT_NE(logger, nullptr);
}

TEST_F(LoggingTest, FrameworkLevelIndependent)
{
    LogConfig cfg;
    cfg.level = "info";
    cfg.framework_level = "debug";
    init_logging(cfg);

    auto apex = spdlog::get("apex");
    auto app = spdlog::get("app");
    EXPECT_EQ(apex->level(), spdlog::level::debug);
    EXPECT_EQ(app->level(), spdlog::level::info);
}

TEST_F(LoggingTest, ConsoleOnlyByDefault)
{
    LogConfig cfg; // console.enabled=true, file.enabled=false
    init_logging(cfg);
    auto logger = spdlog::get("apex");
    // 기본 설정: ConsoleSink 1개만
    EXPECT_EQ(logger->sinks().size(), 1);
}

TEST_F(LoggingTest, FileEnabled)
{
    auto tmp = std::filesystem::temp_directory_path() / "apex_log_test";
    std::filesystem::remove_all(tmp);

    {
        LogConfig cfg;
        cfg.file.enabled = true;
        cfg.file.path = tmp.string();
        cfg.service_name = "test-svc";
        init_logging(cfg);

        auto logger = spdlog::get("app");
        // Console(1) + 6 level sinks = 7
        EXPECT_EQ(logger->sinks().size(), 7);

        logger->info("test message");
        logger->flush();
    }

    shutdown_logging();
    std::filesystem::remove_all(tmp);
}

TEST_F(LoggingTest, ShutdownCleansUp)
{
    LogConfig cfg;
    init_logging(cfg);
    shutdown_logging();
    EXPECT_EQ(spdlog::get("apex"), nullptr);
    EXPECT_EQ(spdlog::get("app"), nullptr);
}

TEST_F(LoggingTest, DoubleInitIsSafe)
{
    LogConfig cfg;
    init_logging(cfg);
    // Second init should not crash or corrupt state
    EXPECT_NO_THROW(init_logging(cfg));
    EXPECT_NE(spdlog::get("apex"), nullptr);
    EXPECT_NE(spdlog::get("app"), nullptr);
}

TEST_F(LoggingTest, InitLoggingWithInvalidLevel)
{
    LogConfig cfg;
    cfg.level = "not_a_valid_level";
    cfg.framework_level = "also_invalid";
    // spdlog::level::from_str returns spdlog::level::off for unrecognized strings.
    // init_logging should not throw -- it gracefully falls back to "off".
    EXPECT_NO_THROW(init_logging(cfg));

    auto apex = spdlog::get("apex");
    auto app = spdlog::get("app");
    ASSERT_NE(apex, nullptr);
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(apex->level(), spdlog::level::off);
    EXPECT_EQ(app->level(), spdlog::level::off);
}

TEST_F(LoggingTest, LoggingAfterShutdownDoesNotCrash)
{
    LogConfig cfg;
    init_logging(cfg);
    shutdown_logging();
    EXPECT_EQ(spdlog::get("apex"), nullptr);
    EXPECT_NO_THROW({
        auto logger = spdlog::get("apex");
        if (logger)
        {
            logger->info("should not reach here");
        }
    });
}

TEST_F(LoggingTest, ExactLevelSinkFiltersExactLevel)
{
    std::ostringstream oss;
    auto ostream_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto exact_info = std::make_shared<exact_level_sink<std::mutex>>(ostream_sink, spdlog::level::info);

    auto logger = std::make_shared<spdlog::logger>("test_exact", exact_info);
    logger->set_level(spdlog::level::trace);

    logger->trace("trace msg");
    logger->debug("debug msg");
    logger->info("info msg");
    logger->warn("warn msg");
    logger->error("error msg");
    logger->flush();

    auto output = oss.str();
    EXPECT_NE(output.find("info msg"), std::string::npos);
    EXPECT_EQ(output.find("trace msg"), std::string::npos);
    EXPECT_EQ(output.find("debug msg"), std::string::npos);
    EXPECT_EQ(output.find("warn msg"), std::string::npos);
    EXPECT_EQ(output.find("error msg"), std::string::npos);
}

TEST_F(LoggingTest, FileEnabledCreatesPerLevelFiles)
{
    auto tmp = std::filesystem::temp_directory_path() / "apex_log_level_test";
    std::filesystem::remove_all(tmp);

    {
        LogConfig cfg;
        cfg.file.enabled = true;
        cfg.file.path = tmp.string();
        cfg.service_name = "test-svc";
        init_logging(cfg);

        auto logger = spdlog::get("app");
        ASSERT_NE(logger, nullptr);
        logger->set_level(spdlog::level::trace);
        logger->trace("trace test");
        logger->info("info test");
        logger->warn("warn test");
        logger->error("error test");
        logger->flush();

        // 서비스 디렉토리 생성 확인
        auto svc_dir = tmp / "test-svc";
        EXPECT_TRUE(std::filesystem::exists(svc_dir));
    }

    shutdown_logging();
    std::filesystem::remove_all(tmp);
}

TEST_F(LoggingTest, AsyncLoggerCreated)
{
    auto tmp = std::filesystem::temp_directory_path() / "apex_async_test";
    std::filesystem::remove_all(tmp); // 이전 테스트 잔여물 정리

    LogConfig cfg;
    cfg.file.enabled = true;
    cfg.file.path = tmp.string();
    cfg.service_name = "async-svc";
    init_logging(cfg);

    auto logger = spdlog::get("app");
    ASSERT_NE(logger, nullptr);
    // Console(1) + 6 level sinks = 7
    EXPECT_GE(logger->sinks().size(), 6);

    // shutdown_logging()은 TearDown에서 호출.
    // async 스레드 파일 핸들 해제까지 Windows에서 지연 가능 → cleanup은 best-effort
    shutdown_logging();
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec); // 핸들 잠금 시 무시
}

TEST_F(LoggingTest, ServiceNameValidation)
{
    LogConfig cfg;
    cfg.service_name = "../escape";
    cfg.file.enabled = true;
    cfg.file.path = (std::filesystem::temp_directory_path() / "apex_validation_test").string();
    EXPECT_THROW(init_logging(cfg), std::invalid_argument);
}
