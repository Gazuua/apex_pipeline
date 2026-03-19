#include <apex/core/config.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace apex::core;

namespace
{

// 테스트용 임시 TOML 파일 생성 헬퍼
class ConfigTest : public ::testing::Test
{
  protected:
    std::filesystem::path tmp_dir_;

    void SetUp() override
    {
        tmp_dir_ = std::filesystem::temp_directory_path() / "apex_config_test";
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::string write_toml(const std::string& filename, const std::string& content)
    {
        auto path = tmp_dir_ / filename;
        std::ofstream ofs(path);
        ofs << content;
        return path.string();
    }
};

} // anonymous namespace

TEST_F(ConfigTest, DefaultsReturnsValidConfig)
{
    auto config = AppConfig::defaults();
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.server.drain_timeout.count(), 25);
    EXPECT_EQ(config.logging.level, "info");
    EXPECT_TRUE(config.logging.console.enabled);
    EXPECT_FALSE(config.logging.file.enabled);
}

TEST_F(ConfigTest, FromFileLoadsServerSection)
{
    auto path = write_toml("server.toml", R"(
[server]
num_cores = 4
drain_timeout_s = 10
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.num_cores, 4);
    EXPECT_EQ(config.server.drain_timeout.count(), 10);
}

TEST_F(ConfigTest, FromFileLoadsLoggingSection)
{
    auto path = write_toml("logging.toml", R"(
[logging]
level = "debug"
framework_level = "warn"
service_name = "test-svc"

[logging.file]
enabled = true
path = "/var/log/apex"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.level, "debug");
    EXPECT_EQ(config.logging.framework_level, "warn");
    EXPECT_EQ(config.logging.service_name, "test-svc");
    EXPECT_TRUE(config.logging.file.enabled);
    EXPECT_EQ(config.logging.file.path, "/var/log/apex");
    EXPECT_FALSE(config.logging.file.json);
}

TEST_F(ConfigTest, MissingFieldsUseDefaults)
{
    auto path = write_toml("minimal.toml", R"(
[server]
num_cores = 2
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.num_cores, 2);
    // 나머지는 기본값
    EXPECT_EQ(config.server.drain_timeout.count(), 25);
    EXPECT_EQ(config.logging.level, "info");
}

TEST_F(ConfigTest, FileNotFoundThrows)
{
    EXPECT_THROW(AppConfig::from_file("/nonexistent/path.toml"), std::runtime_error);
}

TEST_F(ConfigTest, InvalidTomlSyntaxThrows)
{
    auto path = write_toml("bad.toml", "[[[[invalid toml");
    EXPECT_THROW(AppConfig::from_file(path), std::runtime_error);
}

TEST_F(ConfigTest, WrongFieldTypeThrows)
{
    auto path = write_toml("badtype.toml", R"(
[server]
num_cores = "not_a_number"
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}

TEST_F(ConfigTest, UnknownSectionsIgnored)
{
    auto path = write_toml("extra.toml", R"(
[server]
num_cores = 1

[kafka]
brokers = "localhost:9092"

[redis]
host = "localhost"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.num_cores, 1);
    // kafka, redis 섹션은 무시 — 에러 없음
}

TEST_F(ConfigTest, EmptyFileUsesDefaults)
{
    auto path = write_toml("empty.toml", "");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.logging.level, "info");
}

// port 관련 테스트 제거 — ServerConfig::port가 삭제됨 (v0.5, listen<P>(port) API로 대체)

TEST_F(ConfigTest, TickIntervalNegativeThrows)
{
    auto path = write_toml("neg_tick.toml", R"(
[server]
tick_interval_ms = -1
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}

TEST_F(ConfigTest, ServiceNameDefaultEmpty)
{
    auto path = write_toml("no_svc.toml", R"(
[logging]
level = "info"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_TRUE(config.logging.service_name.empty());
}

TEST_F(ConfigTest, AsyncQueueSizeParsed)
{
    auto path = write_toml("async.toml", R"(
[logging.async]
queue_size = 16384
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.async.queue_size, 16384);
}

TEST_F(ConfigTest, AsyncQueueSizeDefaultIs8192)
{
    auto path = write_toml("no_async.toml", "");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.async.queue_size, 8192);
}

TEST_F(ConfigTest, DeprecatedFieldsIgnored)
{
    auto path = write_toml("deprecated.toml", R"(
[logging.file]
enabled = true
max_size_mb = 100
max_files = 3
)");
    // deprecated 필드는 무시 — 에러 없이 파싱 성공
    EXPECT_NO_THROW(AppConfig::from_file(path));
}

TEST_F(ConfigTest, DrainTimeoutNegativeThrows)
{
    auto path = write_toml("neg_drain_timeout.toml", R"(
[server]
drain_timeout_s = -1
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}
