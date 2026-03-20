// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace apex::shared::rate_limit
{

/// Result of a rate limit check.
struct RateLimitResult
{
    bool allowed;
    uint32_t estimated_count;
    uint32_t retry_after_ms; ///< 0 if allowed, else suggested wait time
};

struct RedisRateLimiterConfig
{
    uint32_t default_limit = 100;         ///< 기본 한도 (per window)
    std::chrono::seconds window_size{60}; ///< 윈도우 크기
};

/// Redis-based rate limiter using Lua scripting for atomic check+increment.
/// Used for Per-User and Per-Endpoint rate limiting.
///
/// Thread safety: NOT thread-safe (per-core RedisMultiplexer 사용).
///
/// Usage:
///   RedisRateLimiter limiter(config, multiplexer);
///
///   // Per-User check
///   auto result = co_await limiter.check_user(user_id, now_ms);
///
///   // Per-Endpoint check
///   auto result = co_await limiter.check_endpoint(user_id, msg_id, now_ms);
///
///   // Per-Endpoint with override limit
///   auto result = co_await limiter.check_endpoint(user_id, msg_id, now_ms, 50);
class RedisRateLimiter
{
  public:
    RedisRateLimiter(RedisRateLimiterConfig config, adapters::redis::RedisMultiplexer& multiplexer);

    /// Check per-user rate limit.
    /// @param user_id User identifier.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>> check_user(uint64_t user_id,
                                                                                         uint64_t now_ms);

    /// Check per-endpoint rate limit.
    /// @param user_id User identifier.
    /// @param msg_id Message type ID.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @param limit_override Optional per-endpoint limit override (0 = use default).
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    check_endpoint(uint64_t user_id, uint32_t msg_id, uint64_t now_ms, uint32_t limit_override = 0);

    /// Update configuration at runtime (TOML hot-reload).
    void update_config(RedisRateLimiterConfig config) noexcept;

    /// Get the loaded Lua script SHA1 hash (for EVALSHA).
    [[nodiscard]] std::string_view script_sha() const noexcept
    {
        return script_sha_;
    }

    /// Load the Lua script into Redis (SCRIPT LOAD). Must be called once
    /// after connection is established.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> load_script();

    /// Embedded Lua script source (compiled into binary).
    static constexpr std::string_view LUA_SCRIPT = R"lua(
local cur_key = KEYS[1]
local prev_key = KEYS[2]
local limit = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local now_ms = tonumber(ARGV[3])

local cur_count = tonumber(redis.call('GET', cur_key) or '0')
local prev_count = tonumber(redis.call('GET', prev_key) or '0')

local meta_key = cur_key .. ':meta'
local window_start = tonumber(redis.call('GET', meta_key) or '0')

if window_start == 0 then
    window_start = now_ms
    redis.call('SET', meta_key, tostring(now_ms))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
end

local elapsed = now_ms - window_start

if elapsed >= window_ms then
    local windows_passed = math.floor(elapsed / window_ms)
    if windows_passed == 1 then
        redis.call('SET', prev_key, tostring(cur_count))
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = window_start + window_ms
    else
        redis.call('SET', prev_key, '0')
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        prev_count = 0
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = now_ms
    end
    redis.call('SET', meta_key, tostring(window_start))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
    elapsed = now_ms - window_start
end

local ratio = elapsed / window_ms
if ratio > 1.0 then ratio = 1.0 end
local estimate = prev_count * (1.0 - ratio) + cur_count

if estimate >= limit then
    local retry_after = math.max(0, window_ms - elapsed)
    return {0, math.floor(estimate), math.floor(retry_after)}
end

cur_count = redis.call('INCR', cur_key)
redis.call('PEXPIRE', cur_key, window_ms * 2)

return {1, math.floor(estimate), 0}
)lua";

  private:
    /// Execute the sliding window Lua script.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    execute_lua(std::string_view cur_key, std::string_view prev_key, uint32_t limit, uint64_t now_ms);

    RedisRateLimiterConfig config_;
    adapters::redis::RedisMultiplexer& multiplexer_;
    std::string script_sha_;
};

} // namespace apex::shared::rate_limit
