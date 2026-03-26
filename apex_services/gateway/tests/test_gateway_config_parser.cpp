// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>

#include <apex/core/error_code.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

// ============================================================
// GatewayConfigParser 단위 테스트 — parse_gateway_config()의
// 모든 TOML 섹션/필드를 개별 검증.
// ============================================================

namespace
{

class GatewayConfigParserTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        temp_dir_ = std::filesystem::temp_directory_path() / "apex_test_config_parser";
        std::filesystem::create_directories(temp_dir_);
        config_path_ = (temp_dir_ / "gateway.toml").string();
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    void write_config(const std::string& content)
    {
        std::ofstream ofs(config_path_);
        ofs << content;
        ofs.flush();
    }

    std::filesystem::path temp_dir_;
    std::string config_path_;
};

// --- FullConfig: 모든 섹션이 있는 완전한 TOML ---

TEST_F(GatewayConfigParserTest, FullConfig)
{
    write_config(R"(
[server]
ws_port = 9443
tcp_port = 7700
num_cores = 4

[tls]
cert_file = "/etc/ssl/cert.pem"
key_file = "/etc/ssl/key.pem"
ca_file = "/etc/ssl/ca.pem"

[jwt]
public_key_file = "keys/pub.pem"
algorithm = "RS256"
issuer = "apex-auth"
clock_skew_seconds = 60
sensitive_msg_ids = [2001, 2002, 2003]

[[routes]]
range_begin = 1000
range_end = 1999
kafka_topic = "auth.requests"

[[routes]]
range_begin = 2000
range_end = 2999
kafka_topic = "chat.requests"

[kafka]
brokers = "kafka1:9092,kafka2:9092"
consumer_group = "gw-prod"
response_topic = "gw.responses"

[redis.pubsub]
host = "redis-pubsub"
port = 6380
password = "pubsub-secret"

[redis.auth]
host = "redis-auth"
port = 6381
password = "auth-secret"

[redis.ratelimit]
host = "redis-rl"
port = 6382
password = "rl-secret"

[pubsub]
max_subscriptions_per_session = 100
global_channels = ["pub:global:chat", "pub:global:system"]

[timeouts]
request_timeout_ms = 10000
max_pending_per_core = 32768
heartbeat_timeout_ticks = 600
sweep_interval_ms = 2000

[auth]
blacklist_fail_open = true

[auth.exempt]
LoginRequest = 1000
LogoutRequest = 1002

[rate_limit.ip]
total_limit = 500
window_size_seconds = 30
max_entries = 32768
ttl_multiplier = 3

[rate_limit.user]
default_limit = 50
window_size_seconds = 120

[rate_limit.endpoint]
default_limit = 30
window_size_seconds = 30

[rate_limit.endpoint.overrides]
2001 = 10
2002 = 5
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    // [server]
    EXPECT_EQ(cfg->ws_port, 9443);
    EXPECT_EQ(cfg->tcp_port, 7700);
    EXPECT_EQ(cfg->num_cores, 4u);

    // [tls]
    EXPECT_EQ(cfg->tls.cert_file, "/etc/ssl/cert.pem");
    EXPECT_EQ(cfg->tls.key_file, "/etc/ssl/key.pem");
    EXPECT_EQ(cfg->tls.ca_file, "/etc/ssl/ca.pem");

    // [jwt]
    EXPECT_EQ(cfg->jwt.public_key_file, "keys/pub.pem");
    EXPECT_EQ(cfg->jwt.algorithm, "RS256");
    EXPECT_EQ(cfg->jwt.issuer, "apex-auth");
    EXPECT_EQ(cfg->jwt.clock_skew, std::chrono::seconds{60});
    ASSERT_EQ(cfg->jwt.sensitive_msg_ids.size(), 3u);
    EXPECT_EQ(cfg->jwt.sensitive_msg_ids[0], 2001u);
    EXPECT_EQ(cfg->jwt.sensitive_msg_ids[1], 2002u);
    EXPECT_EQ(cfg->jwt.sensitive_msg_ids[2], 2003u);

    // [[routes]]
    ASSERT_EQ(cfg->routes.size(), 2u);
    EXPECT_EQ(cfg->routes[0].range_begin, 1000u);
    EXPECT_EQ(cfg->routes[0].range_end, 1999u);
    EXPECT_EQ(cfg->routes[0].kafka_topic, "auth.requests");
    EXPECT_EQ(cfg->routes[1].range_begin, 2000u);
    EXPECT_EQ(cfg->routes[1].range_end, 2999u);
    EXPECT_EQ(cfg->routes[1].kafka_topic, "chat.requests");

    // [kafka]
    EXPECT_EQ(cfg->kafka_brokers, "kafka1:9092,kafka2:9092");
    EXPECT_EQ(cfg->kafka_consumer_group, "gw-prod");
    EXPECT_EQ(cfg->kafka_response_topic, "gw.responses");

    // [redis.pubsub]
    EXPECT_EQ(cfg->redis_pubsub_host, "redis-pubsub");
    EXPECT_EQ(cfg->redis_pubsub_port, 6380);
    EXPECT_EQ(cfg->redis_pubsub_password, "pubsub-secret");

    // [redis.auth]
    EXPECT_EQ(cfg->redis_auth_host, "redis-auth");
    EXPECT_EQ(cfg->redis_auth_port, 6381);
    EXPECT_EQ(cfg->redis_auth_password, "auth-secret");

    // [redis.ratelimit]
    EXPECT_EQ(cfg->redis_ratelimit_host, "redis-rl");
    EXPECT_EQ(cfg->redis_ratelimit_port, 6382);
    EXPECT_EQ(cfg->redis_ratelimit_password, "rl-secret");

    // [pubsub]
    EXPECT_EQ(cfg->max_subscriptions_per_session, 100u);
    ASSERT_EQ(cfg->global_channels.size(), 2u);
    EXPECT_EQ(cfg->global_channels[0], "pub:global:chat");
    EXPECT_EQ(cfg->global_channels[1], "pub:global:system");

    // [timeouts]
    EXPECT_EQ(cfg->request_timeout, std::chrono::milliseconds{10000});
    EXPECT_EQ(cfg->max_pending_per_core, 32768u);
    EXPECT_EQ(cfg->heartbeat_timeout_ticks, 600u);
    EXPECT_EQ(cfg->sweep_interval_ms, 2000u);

    // [auth]
    EXPECT_TRUE(cfg->auth.blacklist_fail_open);
    EXPECT_EQ(cfg->auth.auth_exempt_msg_ids.size(), 2u);
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1000));
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1002));

    // [rate_limit.ip]
    EXPECT_EQ(cfg->rate_limit.ip.total_limit, 500u);
    EXPECT_EQ(cfg->rate_limit.ip.window_size_seconds, 30u);
    EXPECT_EQ(cfg->rate_limit.ip.max_entries, 32768u);
    EXPECT_EQ(cfg->rate_limit.ip.ttl_multiplier, 3u);

    // [rate_limit.user]
    EXPECT_EQ(cfg->rate_limit.user.default_limit, 50u);
    EXPECT_EQ(cfg->rate_limit.user.window_size_seconds, 120u);

    // [rate_limit.endpoint]
    EXPECT_EQ(cfg->rate_limit.endpoint.default_limit, 30u);
    EXPECT_EQ(cfg->rate_limit.endpoint.window_size_seconds, 30u);
    ASSERT_EQ(cfg->rate_limit.endpoint.overrides.size(), 2u);
    // TOML table iteration order is unspecified, so sort before checking
    auto overrides = cfg->rate_limit.endpoint.overrides;
    std::sort(overrides.begin(), overrides.end());
    EXPECT_EQ(overrides[0].first, 2001u);
    EXPECT_EQ(overrides[0].second, 10u);
    EXPECT_EQ(overrides[1].first, 2002u);
    EXPECT_EQ(overrides[1].second, 5u);
}

// --- MinimalConfig: [server]만 → 나머지 기본값 ---

TEST_F(GatewayConfigParserTest, MinimalConfig)
{
    write_config(R"(
[server]
ws_port = 8443
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    // Explicit value
    EXPECT_EQ(cfg->ws_port, 8443);

    // Defaults
    EXPECT_EQ(cfg->tcp_port, 0);
    EXPECT_EQ(cfg->num_cores, 1u);
    EXPECT_TRUE(cfg->tls.cert_file.empty());
    EXPECT_TRUE(cfg->tls.key_file.empty());
    EXPECT_TRUE(cfg->tls.ca_file.empty());
    EXPECT_EQ(cfg->jwt.algorithm, "RS256");
    EXPECT_EQ(cfg->jwt.issuer, "apex-auth");
    EXPECT_EQ(cfg->jwt.clock_skew, std::chrono::seconds{30});
    EXPECT_TRUE(cfg->jwt.sensitive_msg_ids.empty());
    EXPECT_TRUE(cfg->routes.empty());
    EXPECT_EQ(cfg->kafka_brokers, "localhost:9092");
    EXPECT_EQ(cfg->kafka_consumer_group, "gateway");
    EXPECT_EQ(cfg->kafka_response_topic, "gateway.responses");
    EXPECT_EQ(cfg->redis_pubsub_host, "localhost");
    EXPECT_EQ(cfg->redis_pubsub_port, 6379);
    EXPECT_TRUE(cfg->redis_pubsub_password.empty());
    EXPECT_EQ(cfg->redis_auth_host, "localhost");
    EXPECT_EQ(cfg->redis_auth_port, 6379);
    EXPECT_TRUE(cfg->redis_auth_password.empty());
    EXPECT_EQ(cfg->redis_ratelimit_host, "localhost");
    EXPECT_EQ(cfg->redis_ratelimit_port, 6379);
    EXPECT_TRUE(cfg->redis_ratelimit_password.empty());
    EXPECT_EQ(cfg->max_subscriptions_per_session, 50u);
    EXPECT_TRUE(cfg->global_channels.empty());
    EXPECT_EQ(cfg->request_timeout, std::chrono::milliseconds{5000});
    EXPECT_EQ(cfg->max_pending_per_core, 65536u);
    EXPECT_EQ(cfg->heartbeat_timeout_ticks, 300u);
    EXPECT_EQ(cfg->sweep_interval_ms, 1000u);
    EXPECT_FALSE(cfg->auth.blacklist_fail_open);
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.empty());
    EXPECT_EQ(cfg->rate_limit.ip.total_limit, 1000u);
    EXPECT_EQ(cfg->rate_limit.ip.window_size_seconds, 60u);
    EXPECT_EQ(cfg->rate_limit.user.default_limit, 100u);
    EXPECT_EQ(cfg->rate_limit.endpoint.default_limit, 60u);
    EXPECT_TRUE(cfg->rate_limit.endpoint.overrides.empty());
}

// --- EmptyFile: 빈 TOML → 모든 기본값으로 파싱 성공 ---

TEST_F(GatewayConfigParserTest, EmptyFile)
{
    write_config("");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    // All defaults
    EXPECT_EQ(cfg->ws_port, 8443);
    EXPECT_EQ(cfg->tcp_port, 0);
    EXPECT_EQ(cfg->num_cores, 1u);
    EXPECT_TRUE(cfg->routes.empty());
    EXPECT_EQ(cfg->kafka_brokers, "localhost:9092");
    EXPECT_FALSE(cfg->auth.blacklist_fail_open);
}

// --- InvalidToml: 잘못된 TOML 문법 → Result error ---

TEST_F(GatewayConfigParserTest, InvalidToml)
{
    write_config("this is {{ not valid toml [[[");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    EXPECT_FALSE(cfg.has_value());
    EXPECT_EQ(cfg.error(), apex::core::ErrorCode::ServiceError);
}

// --- NonExistentFile: 존재하지 않는 파일 → Result error ---

TEST_F(GatewayConfigParserTest, NonExistentFile)
{
    auto cfg = apex::gateway::parse_gateway_config("/nonexistent/path/gateway.toml");
    EXPECT_FALSE(cfg.has_value());
}

// --- RoutesArray: [[routes]] 배열 다중 파싱 검증 ---

TEST_F(GatewayConfigParserTest, RoutesArray)
{
    write_config(R"(
[[routes]]
range_begin = 1000
range_end = 1999
kafka_topic = "auth.requests"

[[routes]]
range_begin = 2000
range_end = 2999
kafka_topic = "chat.requests"

[[routes]]
range_begin = 3000
range_end = 3999
kafka_topic = "game.requests"

[[routes]]
range_begin = 4000
range_end = 4999
kafka_topic = "inventory.requests"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    ASSERT_EQ(cfg->routes.size(), 4u);
    EXPECT_EQ(cfg->routes[0].range_begin, 1000u);
    EXPECT_EQ(cfg->routes[0].range_end, 1999u);
    EXPECT_EQ(cfg->routes[0].kafka_topic, "auth.requests");
    EXPECT_EQ(cfg->routes[1].range_begin, 2000u);
    EXPECT_EQ(cfg->routes[1].range_end, 2999u);
    EXPECT_EQ(cfg->routes[1].kafka_topic, "chat.requests");
    EXPECT_EQ(cfg->routes[2].range_begin, 3000u);
    EXPECT_EQ(cfg->routes[2].range_end, 3999u);
    EXPECT_EQ(cfg->routes[2].kafka_topic, "game.requests");
    EXPECT_EQ(cfg->routes[3].range_begin, 4000u);
    EXPECT_EQ(cfg->routes[3].range_end, 4999u);
    EXPECT_EQ(cfg->routes[3].kafka_topic, "inventory.requests");
}

// --- AuthExempt: [auth.exempt] 테이블 파싱 검증 ---

TEST_F(GatewayConfigParserTest, AuthExempt)
{
    write_config(R"(
[auth]
blacklist_fail_open = true

[auth.exempt]
LoginRequest = 1000
LogoutRequest = 1002
RefreshTokenRequest = 1004
RegisterRequest = 1006
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_TRUE(cfg->auth.blacklist_fail_open);
    EXPECT_EQ(cfg->auth.auth_exempt_msg_ids.size(), 4u);
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1000));
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1002));
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1004));
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.contains(1006));
    EXPECT_FALSE(cfg->auth.auth_exempt_msg_ids.contains(9999));
}

// --- RateLimitEndpointOverrides: [rate_limit.endpoint.overrides] 파싱 ---

TEST_F(GatewayConfigParserTest, RateLimitEndpointOverrides)
{
    write_config(R"(
[rate_limit.endpoint]
default_limit = 60
window_size_seconds = 60

[rate_limit.endpoint.overrides]
2001 = 10
2002 = 5
3001 = 20
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->rate_limit.endpoint.default_limit, 60u);
    EXPECT_EQ(cfg->rate_limit.endpoint.window_size_seconds, 60u);
    ASSERT_EQ(cfg->rate_limit.endpoint.overrides.size(), 3u);

    // Sort to get deterministic order (TOML table iteration order unspecified)
    auto overrides = cfg->rate_limit.endpoint.overrides;
    std::sort(overrides.begin(), overrides.end());
    EXPECT_EQ(overrides[0].first, 2001u);
    EXPECT_EQ(overrides[0].second, 10u);
    EXPECT_EQ(overrides[1].first, 2002u);
    EXPECT_EQ(overrides[1].second, 5u);
    EXPECT_EQ(overrides[2].first, 3001u);
    EXPECT_EQ(overrides[2].second, 20u);
}

// --- RedisPubsubConfig: [redis.pubsub] 필드 + password expand_env ---

TEST_F(GatewayConfigParserTest, RedisPubsubConfig)
{
    write_config(R"(
[redis.pubsub]
host = "redis-cluster.internal"
port = 6380
password = "plain-text-password"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->redis_pubsub_host, "redis-cluster.internal");
    EXPECT_EQ(cfg->redis_pubsub_port, 6380);
    // password goes through expand_env — plain text should pass through unchanged
    EXPECT_EQ(cfg->redis_pubsub_password, "plain-text-password");
}

// --- RedisPubsubExpandEnv: ${VAR:-default} 패턴이 expand_env를 거치는지 ---

TEST_F(GatewayConfigParserTest, RedisPubsubExpandEnvFallback)
{
    // ${NONEXISTENT_VAR_FOR_TEST:-fallback_value} → "fallback_value"
    write_config(R"(
[redis.pubsub]
host = "localhost"
port = 6379
password = "${APEX_TEST_NONEXISTENT_VAR_XYZ:-my_default_pass}"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->redis_pubsub_password, "my_default_pass");
}

// --- RedisAuthConfig: [redis.auth] 필드 검증 ---

TEST_F(GatewayConfigParserTest, RedisAuthConfig)
{
    write_config(R"(
[redis.auth]
host = "redis-auth.internal"
port = 6381
password = "auth-secret"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->redis_auth_host, "redis-auth.internal");
    EXPECT_EQ(cfg->redis_auth_port, 6381);
    EXPECT_EQ(cfg->redis_auth_password, "auth-secret");
}

// --- RedisRateLimitConfig: [redis.ratelimit] 필드 검증 ---

TEST_F(GatewayConfigParserTest, RedisRateLimitConfig)
{
    write_config(R"(
[redis.ratelimit]
host = "redis-rl.internal"
port = 6382
password = "rl-secret"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->redis_ratelimit_host, "redis-rl.internal");
    EXPECT_EQ(cfg->redis_ratelimit_port, 6382);
    EXPECT_EQ(cfg->redis_ratelimit_password, "rl-secret");
}

// --- GlobalChannels: [pubsub.global_channels] 배열 파싱 ---

TEST_F(GatewayConfigParserTest, GlobalChannels)
{
    write_config(R"(
[pubsub]
max_subscriptions_per_session = 200
global_channels = ["pub:global:chat", "pub:global:system", "pub:global:announcements"]
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->max_subscriptions_per_session, 200u);
    ASSERT_EQ(cfg->global_channels.size(), 3u);
    EXPECT_EQ(cfg->global_channels[0], "pub:global:chat");
    EXPECT_EQ(cfg->global_channels[1], "pub:global:system");
    EXPECT_EQ(cfg->global_channels[2], "pub:global:announcements");
}

// --- GlobalChannelsEmpty: 빈 배열 ---

TEST_F(GatewayConfigParserTest, GlobalChannelsEmpty)
{
    write_config(R"(
[pubsub]
max_subscriptions_per_session = 10
global_channels = []
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->max_subscriptions_per_session, 10u);
    EXPECT_TRUE(cfg->global_channels.empty());
}

// --- TlsConfig: [tls] 섹션 파싱 ---

TEST_F(GatewayConfigParserTest, TlsConfig)
{
    write_config(R"(
[tls]
cert_file = "/certs/server.pem"
key_file = "/certs/server-key.pem"
ca_file = "/certs/ca-bundle.pem"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->tls.cert_file, "/certs/server.pem");
    EXPECT_EQ(cfg->tls.key_file, "/certs/server-key.pem");
    EXPECT_EQ(cfg->tls.ca_file, "/certs/ca-bundle.pem");
}

// --- JwtSensitiveMsgIds: sensitive_msg_ids 배열 파싱 ---

TEST_F(GatewayConfigParserTest, JwtSensitiveMsgIds)
{
    write_config(R"(
[jwt]
public_key_file = "keys/pub.pem"
sensitive_msg_ids = [2001, 2005, 2010]
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    ASSERT_EQ(cfg->jwt.sensitive_msg_ids.size(), 3u);
    EXPECT_EQ(cfg->jwt.sensitive_msg_ids[0], 2001u);
    EXPECT_EQ(cfg->jwt.sensitive_msg_ids[1], 2005u);
    EXPECT_EQ(cfg->jwt.sensitive_msg_ids[2], 2010u);
}

// --- JwtSensitiveMsgIdsEmpty: 빈 배열 ---

TEST_F(GatewayConfigParserTest, JwtSensitiveMsgIdsEmpty)
{
    write_config(R"(
[jwt]
public_key_file = "keys/pub.pem"
sensitive_msg_ids = []
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_TRUE(cfg->jwt.sensitive_msg_ids.empty());
}

// --- Timeouts: [timeouts] 섹션 파싱 ---

TEST_F(GatewayConfigParserTest, Timeouts)
{
    write_config(R"(
[timeouts]
request_timeout_ms = 15000
max_pending_per_core = 8192
heartbeat_timeout_ticks = 120
sweep_interval_ms = 500
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->request_timeout, std::chrono::milliseconds{15000});
    EXPECT_EQ(cfg->max_pending_per_core, 8192u);
    EXPECT_EQ(cfg->heartbeat_timeout_ticks, 120u);
    EXPECT_EQ(cfg->sweep_interval_ms, 500u);
}

// --- KafkaConfig: [kafka] 섹션 파싱 ---

TEST_F(GatewayConfigParserTest, KafkaConfig)
{
    write_config(R"(
[kafka]
brokers = "broker1:9092,broker2:9092,broker3:9092"
consumer_group = "gateway-cluster-1"
response_topic = "gateway.responses.v2"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->kafka_brokers, "broker1:9092,broker2:9092,broker3:9092");
    EXPECT_EQ(cfg->kafka_consumer_group, "gateway-cluster-1");
    EXPECT_EQ(cfg->kafka_response_topic, "gateway.responses.v2");
}

// --- RateLimitIpConfig: [rate_limit.ip] 섹션 파싱 ---

TEST_F(GatewayConfigParserTest, RateLimitIpConfig)
{
    write_config(R"(
[rate_limit.ip]
total_limit = 2000
window_size_seconds = 120
max_entries = 131072
ttl_multiplier = 4
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->rate_limit.ip.total_limit, 2000u);
    EXPECT_EQ(cfg->rate_limit.ip.window_size_seconds, 120u);
    EXPECT_EQ(cfg->rate_limit.ip.max_entries, 131072u);
    EXPECT_EQ(cfg->rate_limit.ip.ttl_multiplier, 4u);
}

// --- RateLimitUserConfig: [rate_limit.user] 섹션 파싱 ---

TEST_F(GatewayConfigParserTest, RateLimitUserConfig)
{
    write_config(R"(
[rate_limit.user]
default_limit = 200
window_size_seconds = 300
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_EQ(cfg->rate_limit.user.default_limit, 200u);
    EXPECT_EQ(cfg->rate_limit.user.window_size_seconds, 300u);
}

// --- RoutesWithMissingFields: 라우트에 필드 누락 시 기본값 ---

TEST_F(GatewayConfigParserTest, RoutesWithMissingFields)
{
    write_config(R"(
[[routes]]
kafka_topic = "test.requests"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    ASSERT_EQ(cfg->routes.size(), 1u);
    // Missing range_begin/range_end default to 0
    EXPECT_EQ(cfg->routes[0].range_begin, 0u);
    EXPECT_EQ(cfg->routes[0].range_end, 0u);
    EXPECT_EQ(cfg->routes[0].kafka_topic, "test.requests");
}

// --- NoRoutesSection: [[routes]] 배열 없음 → 빈 라우트 ---

TEST_F(GatewayConfigParserTest, NoRoutesSection)
{
    write_config(R"(
[server]
ws_port = 8443

[kafka]
brokers = "localhost:9092"
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_TRUE(cfg->routes.empty());
}

// --- AuthExemptEmpty: [auth.exempt] 비어있을 때 ---

TEST_F(GatewayConfigParserTest, AuthExemptEmpty)
{
    write_config(R"(
[auth]
blacklist_fail_open = false

[auth.exempt]
)");

    auto cfg = apex::gateway::parse_gateway_config(config_path_);
    ASSERT_TRUE(cfg.has_value());

    EXPECT_FALSE(cfg->auth.blacklist_fail_open);
    EXPECT_TRUE(cfg->auth.auth_exempt_msg_ids.empty());
}

} // namespace
