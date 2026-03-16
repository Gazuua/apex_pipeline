#include <apex/gateway/gateway_pipeline.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>

#include <gtest/gtest.h>

// ============================================================
// GatewayPipeline 테스트 — 파이프라인 흐름 검증.
// Pipeline: [Per-IP Rate Limit] → JWT → [Per-User RL] → [Per-Endpoint RL] → 라우팅
//
// NOTE: GatewayPipeline의 process/authenticate는 boost::asio::awaitable 코루틴.
// 코루틴 기반 메서드는 io_context + co_spawn이 필요하므로,
// 동기 메서드(check_ip_rate_limit)와 구성 검증에 집중.
// ============================================================

namespace {

// --- Per-IP rate limit 동기 테스트 ---

TEST(GatewayPipelineTest, NullRateLimiterAllowsAll) {
    // Rate limiter가 nullptr이면 모든 요청 허용.
    apex::gateway::JwtConfig jwt_cfg;
    jwt_cfg.secret = "test-secret-key-for-hmac256-testing";
    apex::gateway::JwtVerifier verifier(jwt_cfg);

    apex::gateway::GatewayPipeline pipeline(verifier, nullptr, nullptr);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    EXPECT_TRUE(result.has_value());
}

TEST(GatewayPipelineTest, NullRateLimiterMultipleIps) {
    apex::gateway::JwtConfig jwt_cfg;
    jwt_cfg.secret = "test-secret-key-for-hmac256-testing";
    apex::gateway::JwtVerifier verifier(jwt_cfg);

    apex::gateway::GatewayPipeline pipeline(verifier, nullptr, nullptr);

    // Multiple different IPs should all pass
    for (int i = 0; i < 100; ++i) {
        auto ip = "10.0.0." + std::to_string(i);
        auto result = pipeline.check_ip_rate_limit(ip);
        EXPECT_TRUE(result.has_value()) << "Failed for IP: " << ip;
    }
}

// --- AuthState 기본값 ---

TEST(GatewayPipelineTest, AuthStateDefaults) {
    apex::gateway::AuthState state;
    EXPECT_FALSE(state.authenticated);
    EXPECT_EQ(state.user_id, 0u);
    EXPECT_TRUE(state.jti.empty());
}

TEST(GatewayPipelineTest, AuthStateModification) {
    apex::gateway::AuthState state;
    state.authenticated = true;
    state.user_id = 42;
    state.jti = "test-jti-123";

    EXPECT_TRUE(state.authenticated);
    EXPECT_EQ(state.user_id, 42u);
    EXPECT_EQ(state.jti, "test-jti-123");
}

// --- RateLimiter 교체 ---

TEST(GatewayPipelineTest, SetRateLimiterToNull) {
    apex::gateway::JwtConfig jwt_cfg;
    jwt_cfg.secret = "test-secret-key-for-hmac256-testing";
    apex::gateway::JwtVerifier verifier(jwt_cfg);

    apex::gateway::GatewayPipeline pipeline(verifier, nullptr, nullptr);

    // set_rate_limiter(nullptr) should be safe
    pipeline.set_rate_limiter(nullptr);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    EXPECT_TRUE(result.has_value());
}

// --- Pipeline construction ---

TEST(GatewayPipelineTest, ConstructionWithNullBlacklist) {
    apex::gateway::JwtConfig jwt_cfg;
    jwt_cfg.secret = "test-secret-key-for-hmac256-testing";
    apex::gateway::JwtVerifier verifier(jwt_cfg);

    // blacklist null, rate_limiter null — should construct fine
    apex::gateway::GatewayPipeline pipeline(verifier, nullptr);
    auto result = pipeline.check_ip_rate_limit("127.0.0.1");
    EXPECT_TRUE(result.has_value());
}

// --- GatewayConfig 기본값 ---

TEST(GatewayConfigTest, DefaultValues) {
    apex::gateway::GatewayConfig cfg;
    EXPECT_EQ(cfg.ws_port, 8443);
    EXPECT_EQ(cfg.num_cores, 1u);
    EXPECT_EQ(cfg.system_range_end, 999u);
    EXPECT_EQ(cfg.kafka_brokers, "localhost:9092");
    EXPECT_EQ(cfg.kafka_consumer_group, "gateway");
    EXPECT_EQ(cfg.kafka_response_topic, "gateway.responses");
}

TEST(GatewayConfigTest, RateLimitDefaults) {
    apex::gateway::RateLimitConfig rl;
    EXPECT_EQ(rl.ip.total_limit, 1000u);
    EXPECT_EQ(rl.ip.window_size_seconds, 60u);
    EXPECT_EQ(rl.user.default_limit, 100u);
    EXPECT_EQ(rl.endpoint.default_limit, 60u);
}

} // namespace
