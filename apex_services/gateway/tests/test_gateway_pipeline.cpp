#include <apex/gateway/gateway_pipeline.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>

#include <gtest/gtest.h>

#include <fstream>

// ============================================================
// GatewayPipeline 테스트 — 파이프라인 흐름 검증.
// Pipeline: [Per-IP Rate Limit] → JWT → [Per-User RL] → [Per-Endpoint RL] → 라우팅
//
// NOTE: GatewayPipeline의 process/authenticate는 boost::asio::awaitable 코루틴.
// 코루틴 기반 메서드는 io_context + co_spawn이 필요하므로,
// 동기 메서드(check_ip_rate_limit)와 구성 검증에 집중.
//
// RS256 비대칭 키 사용 — JwtVerifier는 public key 파일 경로 기반.
// ============================================================

namespace {

// Minimal 2048-bit RSA public key for testing only (matches test_jwt_verifier.cpp).
constexpr const char* kTestPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Z3VS5JJcds3xfn/yGaX
LMU1VscGwCJqLqMAcGNHMVRMBEzNcSW4pAjTDV2GaFOBXwPQ9qnPBKbACMJVlNLG
SYlHUKna3WROnVuPOPJjCEVR1G7jPNNu5jCFGo9bN5lWLtYGnkJMJKc1H85YYMiP
Yso6cR9YRe3oEjy0VxpEQBR8jM7pMaJBqGHKDO7g7rGIsh2Mi0Qa4H55CiY2TMdG
By/3dOJRH0WEwXFh9cGz3vMHVNPaExVtJbOmFdoF0AdPIFejPO3MH9ou3LjGSIDZ
QxS7KZhhR/3lIDHwVX2pVJjA6nhsNzdqVY+7EHSkj6WWRREP1Cq7CVRwBfSJ3SQh
cwIDAQAB
-----END PUBLIC KEY-----
)";

/// Helper: create a JwtConfig + JwtVerifier with RS256 test public key file.
/// Returns {config, verifier, temp_key_path} tuple.
struct PipelineTestContext {
    apex::gateway::GatewayConfig gw_config;
    std::string temp_key_path;
    std::unique_ptr<apex::gateway::JwtVerifier> verifier;

    PipelineTestContext() {
        temp_key_path = ::testing::TempDir() + "pipeline_test_rs256_pub.pem";
        std::ofstream ofs(temp_key_path);
        ofs << kTestPublicKey;
        ofs.close();

        gw_config.jwt.public_key_file = temp_key_path;
        gw_config.jwt.algorithm = "RS256";
        gw_config.jwt.issuer = "apex-auth";
        verifier = std::make_unique<apex::gateway::JwtVerifier>(gw_config.jwt);
    }

    ~PipelineTestContext() {
        std::remove(temp_key_path.c_str());
    }
};

// --- Per-IP rate limit 동기 테스트 ---

TEST(GatewayPipelineTest, NullRateLimiterAllowsAll) {
    // Rate limiter가 nullptr이면 모든 요청 허용.
    PipelineTestContext ctx;

    apex::gateway::GatewayPipeline pipeline(ctx.gw_config, *ctx.verifier, nullptr, nullptr);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    EXPECT_TRUE(result.has_value());
}

TEST(GatewayPipelineTest, NullRateLimiterMultipleIps) {
    PipelineTestContext ctx;

    apex::gateway::GatewayPipeline pipeline(ctx.gw_config, *ctx.verifier, nullptr, nullptr);

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
    PipelineTestContext ctx;

    apex::gateway::GatewayPipeline pipeline(ctx.gw_config, *ctx.verifier, nullptr, nullptr);

    // set_rate_limiter(nullptr) should be safe
    pipeline.set_rate_limiter(nullptr);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    EXPECT_TRUE(result.has_value());
}

// --- Pipeline construction ---

TEST(GatewayPipelineTest, ConstructionWithNullBlacklist) {
    PipelineTestContext ctx;

    // blacklist null, rate_limiter null — should construct fine
    apex::gateway::GatewayPipeline pipeline(ctx.gw_config, *ctx.verifier, nullptr);
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

// --- AuthConfig 기본값 ---

TEST(GatewayConfigTest, JwtConfigDefaults) {
    apex::gateway::JwtConfig jwt;
    EXPECT_EQ(jwt.algorithm, "RS256");
    EXPECT_EQ(jwt.issuer, "apex-auth");
    EXPECT_EQ(jwt.clock_skew, std::chrono::seconds{30});
    EXPECT_TRUE(jwt.public_key_file.empty());
    EXPECT_TRUE(jwt.sensitive_msg_ids.empty());
}

TEST(GatewayConfigTest, AuthExemptDefaults) {
    apex::gateway::GatewayConfig cfg;
    EXPECT_TRUE(cfg.auth.auth_exempt_msg_ids.empty());
}

TEST(GatewayConfigTest, AuthExemptMsgIds) {
    apex::gateway::GatewayConfig cfg;
    cfg.auth.auth_exempt_msg_ids.insert(1000);
    cfg.auth.auth_exempt_msg_ids.insert(1002);
    cfg.auth.auth_exempt_msg_ids.insert(1004);

    EXPECT_TRUE(cfg.auth.auth_exempt_msg_ids.contains(1000));
    EXPECT_TRUE(cfg.auth.auth_exempt_msg_ids.contains(1002));
    EXPECT_TRUE(cfg.auth.auth_exempt_msg_ids.contains(1004));
    EXPECT_FALSE(cfg.auth.auth_exempt_msg_ids.contains(2000));
}

} // namespace
