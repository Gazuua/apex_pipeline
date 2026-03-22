// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

/// GatewayPipeline 에러 경로 단위 테스트 (BACKLOG-102 + BACKLOG-127).
///
/// Template policy 패턴을 활용: GatewayPipelineBase를 mock 타입으로 인스턴스화하여
/// JWT, Blacklist, RateLimit 의존성 없이 에러 경로를 검증한다.
///
/// 테스트 구조:
///   Mock 타입 (MockVerifier, MockBlacklist, MockLimiter)
///   + io_context::run() 기반 코루틴 하네스
///   + 에러 경로별 테스트 케이스

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/core/session.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_error.hpp>
#include <apex/gateway/gateway_pipeline.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace
{

// ============================================================
// Mock Types
// ============================================================

/// JwtClaims — authenticate()가 참조하는 claims 구조체.
/// 프로덕션 JwtVerifier의 반환 타입과 동일한 필드.
struct MockJwtClaims
{
    uint64_t user_id{0};
    std::string email;
    std::string jti;
    std::chrono::system_clock::time_point expires_at;
};

/// Mock JWT verifier — verify/is_sensitive 결과를 미리 설정.
class MockVerifier
{
  public:
    apex::core::Result<MockJwtClaims> verify(std::string_view /*token*/) const
    {
        if (verify_fail_)
            return apex::core::error(apex::core::ErrorCode::InvalidMessage);
        return claims_;
    }

    bool is_sensitive(uint32_t /*msg_id*/) const noexcept
    {
        return sensitive_;
    }

    void set_verify_success(uint64_t user_id, std::string jti)
    {
        verify_fail_ = false;
        claims_.user_id = user_id;
        claims_.jti = std::move(jti);
    }

    void set_verify_fail()
    {
        verify_fail_ = true;
    }

    void set_sensitive(bool v) noexcept
    {
        sensitive_ = v;
    }

  private:
    bool verify_fail_ = true;
    bool sensitive_ = false;
    MockJwtClaims claims_;
};

/// Mock JWT blacklist — is_blacklisted 결과를 미리 설정.
class MockBlacklist
{
  public:
    boost::asio::awaitable<apex::core::Result<bool>> is_blacklisted(std::string_view /*jti*/)
    {
        if (error_)
            co_return apex::core::error(apex::core::ErrorCode::AdapterError);
        co_return blacklisted_;
    }

    void set_blacklisted(bool v) noexcept
    {
        blacklisted_ = v;
        error_ = false;
    }

    void set_error() noexcept
    {
        error_ = true;
    }

  private:
    bool blacklisted_ = false;
    bool error_ = false;
};

/// Mock rate limiter — check_ip/user/endpoint 결과를 미리 설정.
struct MockRateLimitResult
{
    bool allowed = true;
    uint32_t estimated_count = 0;
    uint32_t retry_after_ms = 0;
};

class MockLimiter
{
  public:
    bool check_ip(std::string_view /*ip*/, std::chrono::steady_clock::time_point /*now*/)
    {
        return ip_allowed_;
    }

    boost::asio::awaitable<apex::core::Result<MockRateLimitResult>> check_user(uint64_t /*user_id*/,
                                                                               uint64_t /*now_ms*/)
    {
        if (user_error_)
            co_return apex::core::error(apex::core::ErrorCode::AdapterError);
        co_return MockRateLimitResult{.allowed = user_allowed_};
    }

    boost::asio::awaitable<apex::core::Result<MockRateLimitResult>>
    check_endpoint(uint64_t /*user_id*/, uint32_t /*msg_id*/, uint64_t /*now_ms*/)
    {
        if (endpoint_error_)
            co_return apex::core::error(apex::core::ErrorCode::AdapterError);
        co_return MockRateLimitResult{.allowed = endpoint_allowed_};
    }

    void set_ip_allowed(bool v) noexcept
    {
        ip_allowed_ = v;
    }
    void set_user_allowed(bool v) noexcept
    {
        user_allowed_ = v;
        user_error_ = false;
    }
    void set_user_error() noexcept
    {
        user_error_ = true;
    }
    void set_endpoint_allowed(bool v) noexcept
    {
        endpoint_allowed_ = v;
        endpoint_error_ = false;
    }
    void set_endpoint_error() noexcept
    {
        endpoint_error_ = true;
    }

  private:
    bool ip_allowed_ = true;
    bool user_allowed_ = true;
    bool user_error_ = false;
    bool endpoint_allowed_ = true;
    bool endpoint_error_ = false;
};

/// Test pipeline type — mock 타입으로 인스턴스화.
using TestPipeline = apex::gateway::GatewayPipelineBase<MockVerifier, MockBlacklist, MockLimiter>;

// ============================================================
// Test Fixture
// ============================================================

class GatewayPipelineErrorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Default: auth not required for msg_id 1000 (exempt)
        config_.auth.auth_exempt_msg_ids.insert(1000);
        // msg_id 2000 = requires auth, sensitive (blacklist check)
        config_.jwt.sensitive_msg_ids.push_back(2000);
    }

    /// 코루틴 테스트 실행 헬퍼 — co_spawn + io_context.run().
    template <typename Coro> void run_coro(Coro&& coro)
    {
        boost::asio::co_spawn(io_ctx_, std::forward<Coro>(coro), boost::asio::detached);
        io_ctx_.run();
        io_ctx_.restart();
    }

    apex::gateway::GatewayConfig config_;
    MockVerifier verifier_;
    MockBlacklist blacklist_;
    MockLimiter limiter_;
    boost::asio::io_context io_ctx_;
};

// ============================================================
// BACKLOG-102: Pipeline 에러 경로 테스트
// ============================================================

// --- IP Rate Limit ---

TEST_F(GatewayPipelineErrorTest, IpRateLimitDenied)
{
    limiter_.set_ip_allowed(false);
    TestPipeline pipeline(config_, verifier_, &blacklist_, &limiter_);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::gateway::GatewayError::RateLimitedIp);
}

TEST_F(GatewayPipelineErrorTest, IpRateLimitAllowed)
{
    limiter_.set_ip_allowed(true);
    TestPipeline pipeline(config_, verifier_, &blacklist_, &limiter_);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    EXPECT_TRUE(result.has_value());
}

TEST_F(GatewayPipelineErrorTest, NullLimiterAllowsIp)
{
    TestPipeline pipeline(config_, verifier_, &blacklist_, nullptr);

    auto result = pipeline.check_ip_rate_limit("192.168.1.1");
    EXPECT_TRUE(result.has_value());
}

// --- JWT Authentication ---

TEST_F(GatewayPipelineErrorTest, AuthExemptMsgIdSkipsAuth)
{
    verifier_.set_verify_fail(); // verify would fail, but exempt skips it
    TestPipeline pipeline(config_, verifier_, nullptr);
    apex::gateway::AuthState state;
    apex::core::WireHeader header{.msg_id = 1000}; // exempt

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(state.authenticated); // exempt doesn't set authenticated
    });
}

TEST_F(GatewayPipelineErrorTest, NoTokenFails)
{
    TestPipeline pipeline(config_, verifier_, nullptr);
    apex::gateway::AuthState state; // token empty
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::gateway::GatewayError::JwtVerifyFailed);
    });
}

TEST_F(GatewayPipelineErrorTest, InvalidTokenFails)
{
    verifier_.set_verify_fail();
    TestPipeline pipeline(config_, verifier_, nullptr);
    apex::gateway::AuthState state;
    state.token = "invalid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::gateway::GatewayError::JwtVerifyFailed);
    });
}

TEST_F(GatewayPipelineErrorTest, ValidTokenSucceeds)
{
    verifier_.set_verify_success(42, "jti-123");
    verifier_.set_sensitive(false);
    TestPipeline pipeline(config_, verifier_, nullptr);
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(state.authenticated);
        EXPECT_EQ(state.user_id, 42u);
        EXPECT_EQ(state.jti, "jti-123");
    });
}

// --- User Rate Limit ---

TEST_F(GatewayPipelineErrorTest, UserRateLimitDenied)
{
    limiter_.set_user_allowed(false);
    TestPipeline pipeline(config_, verifier_, nullptr, &limiter_);

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.check_user_rate_limit(42);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::gateway::GatewayError::RateLimitedUser);
    });
}

TEST_F(GatewayPipelineErrorTest, UserRateLimitRedisErrorFailOpen)
{
    limiter_.set_user_error();
    TestPipeline pipeline(config_, verifier_, nullptr, &limiter_);

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.check_user_rate_limit(42);
        EXPECT_TRUE(result.has_value()); // fail-open: Redis error → allow
    });
}

// --- Endpoint Rate Limit ---

TEST_F(GatewayPipelineErrorTest, EndpointRateLimitDenied)
{
    limiter_.set_endpoint_allowed(false);
    TestPipeline pipeline(config_, verifier_, nullptr, &limiter_);

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.check_endpoint_rate_limit(42, 2000);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::gateway::GatewayError::RateLimitedEndpoint);
    });
}

// ============================================================
// BACKLOG-127: Blacklist fail-open / fail-close 분기 테스트
// ============================================================

TEST_F(GatewayPipelineErrorTest, BlacklistBlocksBlacklistedToken)
{
    verifier_.set_verify_success(42, "jti-blocked");
    verifier_.set_sensitive(true);
    blacklist_.set_blacklisted(true);
    TestPipeline pipeline(config_, verifier_, &blacklist_);
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::gateway::GatewayError::JwtBlacklisted);
    });
}

TEST_F(GatewayPipelineErrorTest, BlacklistAllowsNonBlacklistedToken)
{
    verifier_.set_verify_success(42, "jti-ok");
    verifier_.set_sensitive(true);
    blacklist_.set_blacklisted(false);
    TestPipeline pipeline(config_, verifier_, &blacklist_);
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(state.authenticated);
    });
}

TEST_F(GatewayPipelineErrorTest, BlacklistFailOpen_RedisError_Allows)
{
    verifier_.set_verify_success(42, "jti-fail-open");
    verifier_.set_sensitive(true);
    blacklist_.set_error();
    config_.auth.blacklist_fail_open = true; // fail-open
    TestPipeline pipeline(config_, verifier_, &blacklist_);
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_TRUE(result.has_value()); // fail-open: Redis error → allow
        EXPECT_TRUE(state.authenticated);
    });
}

TEST_F(GatewayPipelineErrorTest, BlacklistFailClose_RedisError_Rejects)
{
    verifier_.set_verify_success(42, "jti-fail-close");
    verifier_.set_sensitive(true);
    blacklist_.set_error();
    config_.auth.blacklist_fail_open = false; // fail-close
    TestPipeline pipeline(config_, verifier_, &blacklist_);
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::gateway::GatewayError::BlacklistCheckFailed);
    });
}

TEST_F(GatewayPipelineErrorTest, NonSensitiveMsgIdSkipsBlacklist)
{
    verifier_.set_verify_success(42, "jti-nonsensitive");
    verifier_.set_sensitive(false);   // msg_id is NOT sensitive
    blacklist_.set_blacklisted(true); // would block if checked
    TestPipeline pipeline(config_, verifier_, &blacklist_);
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_TRUE(result.has_value()); // blacklist not checked for non-sensitive
        EXPECT_TRUE(state.authenticated);
    });
}

TEST_F(GatewayPipelineErrorTest, NullBlacklistSkipsCheck)
{
    verifier_.set_verify_success(42, "jti-null-bl");
    verifier_.set_sensitive(true);
    TestPipeline pipeline(config_, verifier_, nullptr); // blacklist = null
    apex::gateway::AuthState state;
    state.token = "valid-token";
    apex::core::WireHeader header{.msg_id = 2000};

    run_coro([&]() -> boost::asio::awaitable<void> {
        auto result = co_await pipeline.authenticate(nullptr, header, state);
        EXPECT_TRUE(result.has_value()); // null blacklist → skip check
        EXPECT_TRUE(state.authenticated);
    });
}

} // namespace
