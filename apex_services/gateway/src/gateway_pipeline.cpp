// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/gateway_pipeline.hpp>

#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/gateway/gateway_error.hpp>

#include <spdlog/spdlog.h>

namespace apex::gateway
{

GatewayPipeline::GatewayPipeline(const GatewayConfig& config, const JwtVerifier& jwt_verifier, JwtBlacklist* blacklist,
                                 apex::shared::rate_limit::RateLimitFacade* rate_limiter)
    : config_(config)
    , jwt_verifier_(jwt_verifier)
    , blacklist_(blacklist)
    , rate_limiter_(rate_limiter)
{} // atomic<T*> supports direct init

boost::asio::awaitable<apex::core::Result<void>> GatewayPipeline::process(apex::core::SessionPtr session,
                                                                          const apex::core::WireHeader& header,
                                                                          AuthState& state, std::string_view remote_ip)
{
    spdlog::debug("pipeline::process (session={}, msg_id=0x{:04X})", session->id(), header.msg_id);

    // Helper: send service error frame directly to client
    auto send_error = [&](GatewayError gw_err) {
        auto frame = apex::core::ErrorSender::build_error_frame(header.msg_id, apex::core::ErrorCode::ServiceError, "",
                                                                static_cast<uint16_t>(gw_err));
        (void)session->enqueue_write(std::move(frame));
    };

    // Layer 1: Per-IP rate limit (local memory, no I/O)
    auto ip_result = check_ip_rate_limit(remote_ip);
    if (!ip_result)
    {
        spdlog::debug("pipeline::process denied at IP rate-limit (session={}, msg_id=0x{:04X}, ip={})", session->id(),
                      header.msg_id, remote_ip);
        send_error(ip_result.error());
        co_return apex::core::error(apex::core::ErrorCode::ServiceError);
    }

    // JWT authentication
    auto auth_result = co_await authenticate(session, header, state);
    if (!auth_result)
    {
        spdlog::debug("pipeline::process denied at authentication (session={}, msg_id=0x{:04X})", session->id(),
                      header.msg_id);
        send_error(auth_result.error());
        co_return apex::core::error(apex::core::ErrorCode::ServiceError);
    }

    // Layer 2: Per-User rate limit (Redis) -- only for authenticated users
    if (state.authenticated)
    {
        auto user_result = co_await check_user_rate_limit(state.user_id);
        if (!user_result)
        {
            spdlog::debug("pipeline::process denied at user rate-limit (session={}, msg_id=0x{:04X}, user_id={})",
                          session->id(), header.msg_id, state.user_id);
            send_error(user_result.error());
            co_return apex::core::error(apex::core::ErrorCode::ServiceError);
        }

        // Layer 3: Per-Endpoint rate limit (Redis)
        auto ep_result = co_await check_endpoint_rate_limit(state.user_id, header.msg_id);
        if (!ep_result)
        {
            spdlog::debug("pipeline::process denied at endpoint rate-limit (session={}, msg_id=0x{:04X}, user_id={})",
                          session->id(), header.msg_id, state.user_id);
            send_error(ep_result.error());
            co_return apex::core::error(apex::core::ErrorCode::ServiceError);
        }
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<GatewayResult> GatewayPipeline::authenticate(apex::core::SessionPtr /*session*/,
                                                                    const apex::core::WireHeader& header,
                                                                    AuthState& state)
{
    // 1. Config-based whitelist: skip auth for exempt msg_ids (deny-by-default)
    if (config_.auth.auth_exempt_msg_ids.contains(header.msg_id))
    {
        co_return GatewayResult{};
    }

    // 2. Token must be present (set by handshake/login response handler)
    if (state.token.empty())
    {
        spdlog::debug("authenticate: no JWT token for msg_id={}", header.msg_id);
        co_return GatewayResult{std::unexpected(GatewayError::JwtVerifyFailed)};
    }

    // 3. JWT signature + expiry verification (local, zero network cost)
    auto claims_result = jwt_verifier_.verify(state.token);
    if (!claims_result)
    {
        spdlog::debug("authenticate: JWT verify failed for msg_id={}, error={}", header.msg_id,
                      static_cast<uint16_t>(claims_result.error()));
        co_return GatewayResult{std::unexpected(GatewayError::JwtVerifyFailed)};
    }

    // 4. Blacklist check for sensitive msg_ids (Redis, cold path)
    if (blacklist_ && jwt_verifier_.is_sensitive(header.msg_id))
    {
        auto bl_result = co_await blacklist_->is_blacklisted(claims_result->jti);
        if (bl_result && *bl_result)
        {
            spdlog::info("authenticate: blacklisted JWT jti={} for msg_id={}", claims_result->jti, header.msg_id);
            co_return GatewayResult{std::unexpected(GatewayError::JwtBlacklisted)};
        }
        // Redis error on blacklist check — fail-open for resilience
        if (!bl_result)
        {
            spdlog::warn("authenticate: blacklist check failed (Redis error), "
                         "allowing jti={}",
                         claims_result->jti);
        }
    }

    // 5. Authentication successful — update per-session state
    state.authenticated = true;
    state.user_id = claims_result->user_id;
    state.jti = claims_result->jti;

    co_return GatewayResult{};
}

GatewayResult GatewayPipeline::check_ip_rate_limit(std::string_view remote_ip)
{
    auto* limiter = rate_limiter_.load(std::memory_order_acquire);
    if (!limiter)
    {
        return {}; // Rate limiting disabled
    }

    auto now = apex::shared::rate_limit::SlidingWindowCounter::Clock::now();
    if (!limiter->check_ip(remote_ip, now))
    {
        spdlog::debug("Per-IP rate limit exceeded: {}", remote_ip);
        return std::unexpected(GatewayError::RateLimitedIp);
    }

    return {};
}

boost::asio::awaitable<GatewayResult> GatewayPipeline::check_user_rate_limit(uint64_t user_id)
{
    auto* limiter = rate_limiter_.load(std::memory_order_acquire);
    if (!limiter)
    {
        co_return GatewayResult{};
    }

    auto result = co_await limiter->check_user(user_id, now_ms());
    if (!result)
    {
        // Redis error -- log but don't block (fail-open for resilience)
        spdlog::warn("Per-User rate limit check failed (Redis error), allowing: user_id={}", user_id);
        co_return GatewayResult{};
    }

    if (!result->allowed)
    {
        spdlog::debug("Per-User rate limit exceeded: user_id={}, retry_after={}ms", user_id, result->retry_after_ms);
        co_return GatewayResult{std::unexpected(GatewayError::RateLimitedUser)};
    }

    co_return GatewayResult{};
}

boost::asio::awaitable<GatewayResult> GatewayPipeline::check_endpoint_rate_limit(uint64_t user_id, uint32_t msg_id)
{
    auto* limiter = rate_limiter_.load(std::memory_order_acquire);
    if (!limiter)
    {
        co_return GatewayResult{};
    }

    auto result = co_await limiter->check_endpoint(user_id, msg_id, now_ms());
    if (!result)
    {
        // Redis error -- fail-open
        spdlog::warn("Per-Endpoint rate limit check failed (Redis error), allowing: "
                     "user_id={}, msg_id={}",
                     user_id, msg_id);
        co_return GatewayResult{};
    }

    if (!result->allowed)
    {
        spdlog::debug("Per-Endpoint rate limit exceeded: user_id={}, msg_id={}, "
                      "retry_after={}ms",
                      user_id, msg_id, result->retry_after_ms);
        co_return GatewayResult{std::unexpected(GatewayError::RateLimitedEndpoint)};
    }

    co_return GatewayResult{};
}

void GatewayPipeline::set_rate_limiter(apex::shared::rate_limit::RateLimitFacade* limiter) noexcept
{
    rate_limiter_.store(limiter, std::memory_order_release);
}

uint64_t GatewayPipeline::now_ms() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace apex::gateway
