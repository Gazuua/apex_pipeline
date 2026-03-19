#pragma once

#include <apex/shared/rate_limit/endpoint_rate_config.hpp>
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <string_view>

namespace apex::shared::rate_limit
{

/// Facade for the 3-layer rate limiting pipeline.
///
/// Usage in Gateway pipeline:
///   1. check_ip()       -- after TLS, before JWT
///   2. check_user()     -- after JWT verification
///   3. check_endpoint() -- before msg_id routing
///
/// Each layer is independently configurable and can be disabled
/// by setting limit to 0 (bypassed).
class RateLimitFacade
{
  public:
    /// @param ip_limiter Per-IP rate limiter (per-core, local memory).
    /// @param redis_limiter Redis-based rate limiter (Per-User + Per-Endpoint).
    /// @param endpoint_config Endpoint-specific overrides.
    RateLimitFacade(PerIpRateLimiter& ip_limiter, RedisRateLimiter& redis_limiter, EndpointRateConfig endpoint_config);

    /// Layer 1: Per-IP check (local memory, no I/O).
    /// @param ip Client IP address.
    /// @param now Time point for sliding window.
    /// @return true if allowed.
    [[nodiscard]] bool check_ip(std::string_view ip, SlidingWindowCounter::TimePoint now);

    /// Layer 2: Per-User check (Redis Lua).
    /// @param user_id Authenticated user ID.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>> check_user(uint64_t user_id,
                                                                                         uint64_t now_ms);

    /// Layer 3: Per-Endpoint check (Redis Lua, with msg_id override).
    /// @param user_id Authenticated user ID.
    /// @param msg_id Message type ID.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    check_endpoint(uint64_t user_id, uint32_t msg_id, uint64_t now_ms);

    /// Update endpoint config at runtime (TOML hot-reload).
    void update_endpoint_config(EndpointRateConfig config) noexcept;

  private:
    PerIpRateLimiter& ip_limiter_;
    RedisRateLimiter& redis_limiter_;
    EndpointRateConfig endpoint_config_;
};

} // namespace apex::shared::rate_limit
