#include <apex/gateway/gateway_pipeline.hpp>

#include <apex/core/error_code.hpp>

#include <spdlog/spdlog.h>

namespace apex::gateway {

GatewayPipeline::GatewayPipeline(const JwtVerifier& jwt_verifier,
                                 JwtBlacklist* blacklist,
                                 apex::shared::rate_limit::RateLimitFacade* rate_limiter)
    : jwt_verifier_(jwt_verifier),
      blacklist_(blacklist),
      rate_limiter_(rate_limiter) {}

boost::asio::awaitable<apex::core::Result<void>>
GatewayPipeline::process(apex::core::SessionPtr session,
                         const apex::core::WireHeader& header,
                         AuthState& state,
                         std::string_view remote_ip) {
    // Layer 1: Per-IP rate limit (local memory, no I/O)
    auto ip_result = check_ip_rate_limit(remote_ip);
    if (!ip_result) {
        co_return std::unexpected(ip_result.error());
    }

    // JWT authentication
    auto auth_result = co_await authenticate(session, header, state);
    if (!auth_result) {
        co_return std::unexpected(auth_result.error());
    }

    // Layer 2: Per-User rate limit (Redis) -- only for authenticated users
    if (state.authenticated) {
        auto user_result = co_await check_user_rate_limit(state.user_id);
        if (!user_result) {
            co_return std::unexpected(user_result.error());
        }

        // Layer 3: Per-Endpoint rate limit (Redis)
        auto ep_result = co_await check_endpoint_rate_limit(state.user_id, header.msg_id);
        if (!ep_result) {
            co_return std::unexpected(ep_result.error());
        }
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayPipeline::authenticate(apex::core::SessionPtr /*session*/,
                               const apex::core::WireHeader& /*header*/,
                               AuthState& /*state*/) {
    // TODO: JWT verification logic (existing from Plan 1)
    // Login requests (system msg_id range) skip authentication.
    co_return apex::core::ok();
}

apex::core::Result<void>
GatewayPipeline::check_ip_rate_limit(std::string_view remote_ip) {
    if (!rate_limiter_) {
        return apex::core::ok();  // Rate limiting disabled
    }

    auto now = apex::shared::rate_limit::SlidingWindowCounter::Clock::now();
    if (!rate_limiter_->check_ip(remote_ip, now)) {
        spdlog::debug("Per-IP rate limit exceeded: {}", remote_ip);
        return apex::core::error(apex::core::ErrorCode::RateLimitedIp);
    }

    return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayPipeline::check_user_rate_limit(uint64_t user_id) {
    if (!rate_limiter_) {
        co_return apex::core::ok();
    }

    auto result = co_await rate_limiter_->check_user(user_id, now_ms());
    if (!result) {
        // Redis error -- log but don't block (fail-open for resilience)
        spdlog::warn("Per-User rate limit check failed (Redis error), allowing: user_id={}",
                     user_id);
        co_return apex::core::ok();
    }

    if (!result->allowed) {
        spdlog::debug("Per-User rate limit exceeded: user_id={}, retry_after={}ms",
                      user_id, result->retry_after_ms);
        co_return apex::core::error(apex::core::ErrorCode::RateLimitedUser);
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayPipeline::check_endpoint_rate_limit(uint64_t user_id, uint32_t msg_id) {
    if (!rate_limiter_) {
        co_return apex::core::ok();
    }

    auto result = co_await rate_limiter_->check_endpoint(user_id, msg_id, now_ms());
    if (!result) {
        // Redis error -- fail-open
        spdlog::warn("Per-Endpoint rate limit check failed (Redis error), allowing: "
                     "user_id={}, msg_id={}", user_id, msg_id);
        co_return apex::core::ok();
    }

    if (!result->allowed) {
        spdlog::debug("Per-Endpoint rate limit exceeded: user_id={}, msg_id={}, "
                      "retry_after={}ms", user_id, msg_id, result->retry_after_ms);
        co_return apex::core::error(apex::core::ErrorCode::RateLimitedEndpoint);
    }

    co_return apex::core::ok();
}

void GatewayPipeline::set_rate_limiter(
    apex::shared::rate_limit::RateLimitFacade* limiter) noexcept {
    rate_limiter_ = limiter;
}

uint64_t GatewayPipeline::now_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
        .count());
}

} // namespace apex::gateway
