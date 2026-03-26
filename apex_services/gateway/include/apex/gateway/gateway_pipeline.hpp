// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/result.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_error.hpp>
#include <apex/shared/secure_string.hpp>

#include <apex/core/scoped_logger.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>

namespace apex::gateway
{

/// Per-session authentication state.
/// Attached to Session as user_data or managed in separate map.
struct AuthState
{
    bool authenticated = false;
    uint64_t user_id = 0;
    std::string jti;                  // JWT ID (for blacklist check)
    apex::shared::SecureString token; // JWT token (set by handshake/login response handler)
};

/// Gateway request pipeline (template policy pattern).
///
/// Template parameters allow compile-time dependency injection:
///   - VerifierT: JWT verifier — verify(token) → Result<JwtClaims>, is_sensitive(msg_id) → bool
///   - BlacklistT: JWT blacklist — is_blacklisted(jti) → awaitable<Result<bool>>
///   - LimiterT: Rate limit facade — check_ip/check_user/check_endpoint
///
/// Production uses concrete types (zero-cost, inline). Tests use mock types.
///
/// Processing order:
///   [Per-IP rate limit] -> JWT verification -> [Redis blacklist]
///   -> [Per-User rate limit] -> [Per-Endpoint rate limit] -> routing
template <typename VerifierT, typename BlacklistT, typename LimiterT> class GatewayPipelineBase
{
  public:
    /// @param config Gateway configuration (auth exempt list, etc.).
    /// @param jwt_verifier JWT signature verifier.
    /// @param blacklist JWT blacklist checker (nullable -- no Redis connection).
    /// @param rate_limiter 3-layer rate limit facade (nullable -- rate limiting disabled).
    GatewayPipelineBase(const GatewayConfig& config, const VerifierT& jwt_verifier, BlacklistT* blacklist,
                        LimiterT* rate_limiter = nullptr)
        : config_(config)
        , jwt_verifier_(jwt_verifier)
        , blacklist_(blacklist)
        , rate_limiter_{rate_limiter}
    {}

    /// Full pipeline check for a request.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> process(apex::core::SessionPtr session,
                                                                           const apex::core::WireHeader& header,
                                                                           AuthState& state, std::string_view remote_ip)
    {
        logger_.debug(session, header.msg_id, "pipeline::process");

        auto send_error = [&](GatewayError gw_err) {
            auto frame = apex::core::ErrorSender::build_error_frame(header.msg_id, apex::core::ErrorCode::ServiceError,
                                                                    "", static_cast<uint16_t>(gw_err));
            (void)session->enqueue_write(std::move(frame));
        };

        // Layer 1: Per-IP rate limit (local memory, no I/O)
        auto ip_result = check_ip_rate_limit(remote_ip);
        if (!ip_result)
        {
            logger_.debug(session, header.msg_id, "pipeline::process denied at IP rate-limit (ip={})", remote_ip);
            send_error(ip_result.error());
            co_return apex::core::error(apex::core::ErrorCode::ServiceError);
        }

        // JWT authentication
        auto auth_result = co_await authenticate(session, header, state);
        if (!auth_result)
        {
            logger_.debug(session, header.msg_id, "pipeline::process denied at authentication");
            send_error(auth_result.error());
            co_return apex::core::error(apex::core::ErrorCode::ServiceError);
        }

        // Layer 2 + 3: Per-User + Per-Endpoint rate limit (Redis)
        if (state.authenticated)
        {
            auto user_result = co_await check_user_rate_limit(state.user_id);
            if (!user_result)
            {
                logger_.debug(session, header.msg_id, "pipeline::process denied at user rate-limit (user_id={})",
                              state.user_id);
                send_error(user_result.error());
                co_return apex::core::error(apex::core::ErrorCode::ServiceError);
            }

            auto ep_result = co_await check_endpoint_rate_limit(state.user_id, header.msg_id);
            if (!ep_result)
            {
                logger_.debug(session, header.msg_id, "pipeline::process denied at endpoint rate-limit (user_id={})",
                              state.user_id);
                send_error(ep_result.error());
                co_return apex::core::error(apex::core::ErrorCode::ServiceError);
            }
        }
        else if (config_.auth.auth_exempt_msg_ids.contains(header.msg_id))
        {
            // BACKLOG-248: Auth-exempt messages get per-endpoint rate limit keyed by IP.
            // Prevents brute-force via LoginRequest etc. (e.g., Login is 10/60s per IP).
            auto ip_key = fnv1a_hash(remote_ip);
            auto ep_result = co_await check_endpoint_rate_limit(ip_key, header.msg_id);
            if (!ep_result)
            {
                logger_.debug(session, header.msg_id, "pipeline::process denied at endpoint rate-limit (exempt, ip={})",
                              remote_ip);
                send_error(ep_result.error());
                co_return apex::core::error(apex::core::ErrorCode::ServiceError);
            }
        }

        co_return apex::core::ok();
    }

    /// Authentication check only (called for every message).
    [[nodiscard]] boost::asio::awaitable<GatewayResult>
    authenticate(apex::core::SessionPtr /*session*/, const apex::core::WireHeader& header, AuthState& state)
    {
        // 1. Config-based whitelist: skip auth for exempt msg_ids (deny-by-default)
        if (config_.auth.auth_exempt_msg_ids.contains(header.msg_id))
        {
            co_return GatewayResult{};
        }

        // 2. Token must be present (set by handshake/login response handler)
        if (state.token.empty())
        {
            logger_.debug("authenticate: no JWT token for msg_id={}", header.msg_id);
            co_return GatewayResult{std::unexpected(GatewayError::JwtVerifyFailed)};
        }

        // 3. JWT signature + expiry verification (local, zero network cost)
        auto claims_result = jwt_verifier_.verify(state.token.view());
        if (!claims_result)
        {
            logger_.debug("authenticate: JWT verify failed for msg_id={}, error={}", header.msg_id,
                          static_cast<uint16_t>(claims_result.error()));
            co_return GatewayResult{std::unexpected(GatewayError::JwtVerifyFailed)};
        }

        // 4. Blacklist check for sensitive msg_ids (Redis, cold path)
        if (blacklist_ && jwt_verifier_.is_sensitive(header.msg_id))
        {
            auto bl_result = co_await blacklist_->is_blacklisted(claims_result->jti);
            if (bl_result && *bl_result)
            {
                logger_.info("authenticate: blacklisted JWT jti={} for msg_id={}", claims_result->jti, header.msg_id);
                co_return GatewayResult{std::unexpected(GatewayError::JwtBlacklisted)};
            }
            if (!bl_result)
            {
                if (config_.auth.blacklist_fail_open)
                {
                    logger_.warn("authenticate: blacklist check failed (Redis error), allowing jti={}",
                                 claims_result->jti);
                }
                else
                {
                    logger_.warn("authenticate: blacklist check failed (Redis error), rejecting jti={}",
                                 claims_result->jti);
                    co_return GatewayResult{std::unexpected(GatewayError::BlacklistCheckFailed)};
                }
            }
        }

        // 5. Authentication successful — update per-session state
        state.authenticated = true;
        state.user_id = claims_result->user_id;
        state.jti = claims_result->jti;

        co_return GatewayResult{};
    }

    /// Per-IP rate limit check (Layer 1, local memory, no I/O).
    [[nodiscard]] GatewayResult check_ip_rate_limit(std::string_view remote_ip)
    {
        if (!rate_limiter_)
        {
            return {};
        }

        auto now = std::chrono::steady_clock::now();
        if (!rate_limiter_->check_ip(remote_ip, now))
        {
            logger_.debug("Per-IP rate limit exceeded: {}", remote_ip);
            return std::unexpected(GatewayError::RateLimitedIp);
        }

        return {};
    }

    /// Per-User rate limit check (Layer 2, Redis).
    [[nodiscard]] boost::asio::awaitable<GatewayResult> check_user_rate_limit(uint64_t user_id)
    {
        if (!rate_limiter_)
        {
            co_return GatewayResult{};
        }

        auto result = co_await rate_limiter_->check_user(user_id, now_ms());
        if (!result)
        {
            logger_.warn("Per-User rate limit check failed (Redis error), allowing: user_id={}", user_id);
            co_return GatewayResult{};
        }

        if (!result->allowed)
        {
            logger_.debug("Per-User rate limit exceeded: user_id={}, retry_after={}ms", user_id,
                          result->retry_after_ms);
            co_return GatewayResult{std::unexpected(GatewayError::RateLimitedUser)};
        }

        co_return GatewayResult{};
    }

    /// Per-Endpoint rate limit check (Layer 3, Redis).
    [[nodiscard]] boost::asio::awaitable<GatewayResult> check_endpoint_rate_limit(uint64_t user_id, uint32_t msg_id)
    {
        if (!rate_limiter_)
        {
            co_return GatewayResult{};
        }

        auto result = co_await rate_limiter_->check_endpoint(user_id, msg_id, now_ms());
        if (!result)
        {
            logger_.warn("Per-Endpoint rate limit check failed (Redis error), allowing: "
                         "user_id={}, msg_id={}",
                         user_id, msg_id);
            co_return GatewayResult{};
        }

        if (!result->allowed)
        {
            logger_.debug("Per-Endpoint rate limit exceeded: user_id={}, msg_id={}, "
                          "retry_after={}ms",
                          user_id, msg_id, result->retry_after_ms);
            co_return GatewayResult{std::unexpected(GatewayError::RateLimitedEndpoint)};
        }

        co_return GatewayResult{};
    }

    /// Set rate limiter reference (initialization only — call during on_wire before processing starts).
    /// For runtime config changes, use RateLimitFacade::update_endpoint_config() instead.
    void set_rate_limiter(LimiterT* limiter) noexcept
    {
        rate_limiter_ = limiter;
    }

  private:
    [[nodiscard]] static uint64_t fnv1a_hash(std::string_view s) noexcept
    {
        uint64_t h = 14695981039346656037ULL;
        for (char c : s)
            h = (h ^ static_cast<uint64_t>(static_cast<unsigned char>(c))) * 1099511628211ULL;
        return h;
    }

    [[nodiscard]] static uint64_t now_ms() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    const GatewayConfig& config_;
    const VerifierT& jwt_verifier_;
    BlacklistT* blacklist_;
    LimiterT* rate_limiter_;
    apex::core::ScopedLogger logger_{"GatewayPipeline", apex::core::ScopedLogger::NO_CORE, "app"};
};

} // namespace apex::gateway

// --- Production type alias ---
// 프로덕션 코드는 이 헤더 대신 gateway_pipeline_production.hpp를 include하여
// GatewayPipeline using alias를 사용한다.
// 테스트 코드는 이 헤더만 include하고 mock 타입으로 GatewayPipelineBase를 직접 인스턴스화한다.
//
// gateway_pipeline.hpp          = template class만 (mock 테스트용)
// gateway_pipeline_production.hpp = template + concrete includes + using alias (프로덕션용)
