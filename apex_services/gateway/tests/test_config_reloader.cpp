#include <apex/gateway/config_reloader.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>
#include <apex/gateway/route_table.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

// ============================================================
// ConfigReloader 테스트 — 설정 변경 감지 → 교체 콜백 호출.
// FileWatcher 기반 (polling)이므로, 콜백 메커니즘 +
// parse_gateway_config의 라운드트립을 검증.
// ============================================================

namespace {

// Minimal valid TOML for gateway config (RS256 — no HS256 secret)
static const std::string kMinimalToml = R"(
[server]
ws_port = 8443
num_cores = 2

[jwt]
public_key_file = "keys/gateway_rs256_pub.pem"
algorithm = "RS256"
issuer = "apex-auth"

[[routes]]
range_begin = 1000
range_end = 1999
kafka_topic = "auth.requests"

[[routes]]
range_begin = 2000
range_end = 2999
kafka_topic = "chat.requests"
)";

static const std::string kUpdatedToml = R"(
[server]
ws_port = 9443
num_cores = 4

[jwt]
public_key_file = "keys/gateway_rs256_pub_v2.pem"
algorithm = "RS256"
issuer = "apex-auth"

[auth.exempt]
LoginRequest = 1000
LogoutRequest = 1002
RefreshTokenRequest = 10

[[routes]]
range_begin = 1000
range_end = 1999
kafka_topic = "auth.v2.requests"

[[routes]]
range_begin = 2000
range_end = 2999
kafka_topic = "chat.requests"

[[routes]]
range_begin = 3000
range_end = 3999
kafka_topic = "game.requests"
)";

class ConfigReloaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp config file
        temp_dir_ = std::filesystem::temp_directory_path() / "apex_test_config";
        std::filesystem::create_directories(temp_dir_);
        config_path_ = (temp_dir_ / "gateway.toml").string();

        write_config(kMinimalToml);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    void write_config(const std::string& content) {
        std::ofstream ofs(config_path_);
        ofs << content;
        ofs.flush();
    }

    std::filesystem::path temp_dir_;
    std::string config_path_;
};

// --- parse_gateway_config 라운드트립 ---

TEST_F(ConfigReloaderTest, ParseMinimalConfig) {
    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->ws_port, 8443);
    EXPECT_EQ(cfg->num_cores, 2u);
    EXPECT_EQ(cfg->jwt.public_key_file, "keys/gateway_rs256_pub.pem");
    EXPECT_EQ(cfg->jwt.algorithm, "RS256");
    EXPECT_EQ(cfg->jwt.issuer, "apex-auth");
    EXPECT_EQ(cfg->routes.size(), 2u);
    EXPECT_EQ(cfg->routes[0].kafka_topic, "auth.requests");
    EXPECT_EQ(cfg->routes[1].kafka_topic, "chat.requests");
    // Minimal config has no [auth.exempt] — empty set (deny-by-default)
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.empty());
}

TEST_F(ConfigReloaderTest, ParseUpdatedConfig) {
    write_config(kUpdatedToml);

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->ws_port, 9443);
    EXPECT_EQ(cfg->num_cores, 4u);
    EXPECT_EQ(cfg->jwt.public_key_file, "keys/gateway_rs256_pub_v2.pem");
    EXPECT_EQ(cfg->routes.size(), 3u);
    EXPECT_EQ(cfg->routes[0].kafka_topic, "auth.v2.requests");
    EXPECT_EQ(cfg->routes[2].kafka_topic, "game.requests");

    // [auth.exempt] whitelist
    EXPECT_EQ(cfg->auth.auth_exempt_msg_ids.size(), 3u);
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1000));
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1002));
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(10));
    EXPECT_FALSE(cfg->auth.auth_exempt_msg_ids.contains(2000));
}

TEST_F(ConfigReloaderTest, ParseInvalidConfigFails) {
    write_config("this is not valid toml {{{{");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    EXPECT_FALSE(cfg.has_value());
    EXPECT_EQ(cfg.error(), apex::core::ErrorCode::ConfigParseFailed);
}

TEST_F(ConfigReloaderTest, ParseNonExistentFileFails) {
    auto cfg = apex::gateway::parse_gateway_config("/nonexistent/path/to/gateway.toml");
    EXPECT_FALSE(cfg.has_value());
}

// --- RouteTable build from parsed config ---

TEST_F(ConfigReloaderTest, BuildRouteTableFromParsedConfig) {
    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    auto table = apex::gateway::RouteTable::build(cfg->routes);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->size(), 2u);

    EXPECT_EQ(table->resolve(1500).value(), "auth.requests");
    EXPECT_EQ(table->resolve(2500).value(), "chat.requests");
}

// --- Callback 메커니즘 직접 시뮬레이션 ---

TEST_F(ConfigReloaderTest, RouteUpdateCallbackInvoked) {
    // ConfigReloader의 on_file_changed가 내부적으로 하는 일을 직접 재현:
    // 1. parse config
    // 2. build route table
    // 3. invoke callback

    apex::gateway::RouteTablePtr received_table;
    auto callback = [&received_table](apex::gateway::RouteTablePtr table) {
        received_table = std::move(table);
    };

    // Simulate what on_file_changed does
    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    auto table = apex::gateway::RouteTable::build(cfg->routes);
    ASSERT_TRUE(table.has_value());

    auto ptr = std::make_shared<const apex::gateway::RouteTable>(std::move(*table));
    callback(ptr);

    ASSERT_NE(received_table, nullptr);
    EXPECT_EQ(received_table->size(), 2u);
    EXPECT_EQ(received_table->resolve(1000).value(), "auth.requests");
}

TEST_F(ConfigReloaderTest, RateLimitCallbackInvoked) {
    apex::gateway::RateLimitConfig received_config;
    bool callback_invoked = false;

    auto callback = [&](const apex::gateway::RateLimitConfig& config) {
        received_config = config;
        callback_invoked = true;
    };

    // Parse config (uses defaults for rate_limit since not specified in minimal TOML)
    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    callback(cfg->rate_limit);

    EXPECT_TRUE(callback_invoked);
    // Default values
    EXPECT_EQ(received_config.ip.total_limit, 1000u);
}

// --- 설정 변경 시 새 RouteTable 생성 ---

TEST_F(ConfigReloaderTest, ConfigChangeProducesNewRouteTable) {
    // Initial parse
    auto cfg1 = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg1.has_value());
    auto table1 = apex::gateway::RouteTable::build(cfg1->routes);
    ASSERT_TRUE(table1.has_value());
    EXPECT_EQ(table1->size(), 2u);

    // Update config
    write_config(kUpdatedToml);

    // Re-parse
    auto cfg2 = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg2.has_value());
    auto table2 = apex::gateway::RouteTable::build(cfg2->routes);
    ASSERT_TRUE(table2.has_value());
    EXPECT_EQ(table2->size(), 3u);

    // New route should exist
    EXPECT_TRUE(table2->resolve(3500).has_value());
    EXPECT_EQ(table2->resolve(3500).value(), "game.requests");

    // Updated topic
    EXPECT_EQ(table2->resolve(1500).value(), "auth.v2.requests");
}

} // namespace
