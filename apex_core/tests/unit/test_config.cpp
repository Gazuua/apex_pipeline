#include <apex/core/config.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

using namespace apex::core;

namespace {

// 테스트용 임시 TOML 파일 생성 헬퍼
class ConfigTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "apex_config_test";
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::string write_toml(const std::string& filename, const std::string& content) {
        auto path = tmp_dir_ / filename;
        std::ofstream ofs(path);
        ofs << content;
        return path.string();
    }
};

} // anonymous namespace

TEST_F(ConfigTest, DefaultsReturnsValidConfig) {
    auto config = AppConfig::defaults();
    EXPECT_EQ(config.server.port, 9000);
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.server.drain_timeout.count(), 25);
    EXPECT_EQ(config.logging.level, "info");
    EXPECT_TRUE(config.logging.console.enabled);
    EXPECT_FALSE(config.logging.file.enabled);
}

TEST_F(ConfigTest, FromFileLoadsServerSection) {
    auto path = write_toml("server.toml", R"(
[server]
port = 8080
num_cores = 4
drain_timeout_s = 10
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 8080);
    EXPECT_EQ(config.server.num_cores, 4);
    EXPECT_EQ(config.server.drain_timeout.count(), 10);
}

TEST_F(ConfigTest, FromFileLoadsLoggingSection) {
    auto path = write_toml("logging.toml", R"(
[logging]
level = "debug"
framework_level = "warn"

[logging.file]
enabled = true
path = "/var/log/apex.log"
max_size_mb = 50
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.level, "debug");
    EXPECT_EQ(config.logging.framework_level, "warn");
    EXPECT_TRUE(config.logging.file.enabled);
    EXPECT_EQ(config.logging.file.path, "/var/log/apex.log");
    EXPECT_EQ(config.logging.file.max_size_mb, 50);
    // 누락 필드는 기본값
    EXPECT_EQ(config.logging.file.max_files, 3);
    EXPECT_TRUE(config.logging.file.json);
}

TEST_F(ConfigTest, MissingFieldsUseDefaults) {
    auto path = write_toml("minimal.toml", R"(
[server]
port = 7777
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 7777);
    // 나머지는 기본값
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.server.drain_timeout.count(), 25);
    EXPECT_EQ(config.logging.level, "info");
}

TEST_F(ConfigTest, FileNotFoundThrows) {
    EXPECT_THROW(AppConfig::from_file("/nonexistent/path.toml"), std::runtime_error);
}

TEST_F(ConfigTest, InvalidTomlSyntaxThrows) {
    auto path = write_toml("bad.toml", "[[[[invalid toml");
    EXPECT_THROW(AppConfig::from_file(path), std::runtime_error);
}

TEST_F(ConfigTest, WrongFieldTypeThrows) {
    auto path = write_toml("badtype.toml", R"(
[server]
port = "not_a_number"
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}

TEST_F(ConfigTest, UnknownSectionsIgnored) {
    auto path = write_toml("extra.toml", R"(
[server]
port = 9000

[kafka]
brokers = "localhost:9092"

[redis]
host = "localhost"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 9000);
    // kafka, redis 섹션은 무시 — 에러 없음
}

TEST_F(ConfigTest, EmptyFileUsesDefaults) {
    auto path = write_toml("empty.toml", "");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 9000);
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.logging.level, "info");
}

TEST_F(ConfigTest, PortOutOfRangeThrows) {
    auto path = write_toml("bigport.toml", R"(
[server]
port = 99999
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}

TEST_F(ConfigTest, NegativePortThrows) {
    auto path = write_toml("negport.toml", R"(
[server]
port = -1
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}
