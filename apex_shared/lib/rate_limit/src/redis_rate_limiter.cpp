// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/rate_limit/redis_rate_limiter.hpp>

#include <format>
#include <string>

namespace apex::shared::rate_limit
{

RedisRateLimiter::RedisRateLimiter(RedisRateLimiterConfig config, adapters::redis::RedisMultiplexer& multiplexer)
    : config_(config)
    , multiplexer_(multiplexer)
{}

boost::asio::awaitable<apex::core::Result<RateLimitResult>> RedisRateLimiter::check_user(uint64_t user_id,
                                                                                         uint64_t now_ms)
{
    auto cur_key = std::format("rl:user:{}:cur", user_id);
    auto prev_key = std::format("rl:user:{}:prev", user_id);
    co_return co_await execute_lua(cur_key, prev_key, config_.default_limit, now_ms);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::check_endpoint(uint64_t user_id, uint32_t msg_id, uint64_t now_ms, uint32_t limit_override)
{
    auto cur_key = std::format("rl:ep:{}:{}:cur", user_id, msg_id);
    auto prev_key = std::format("rl:ep:{}:{}:prev", user_id, msg_id);
    auto limit = (limit_override > 0) ? limit_override : config_.default_limit;
    co_return co_await execute_lua(cur_key, prev_key, limit, now_ms);
}

void RedisRateLimiter::update_config(RedisRateLimiterConfig config) noexcept
{
    config_ = config;
}

boost::asio::awaitable<apex::core::Result<void>> RedisRateLimiter::load_script()
{
    // Use parameterized command to pass Lua script as a proper RESP bulk string.
    // The deprecated command(string_view) splits on whitespace, breaking multi-line scripts.
    auto script = std::string(LUA_SCRIPT);
    auto result = co_await multiplexer_.command("SCRIPT LOAD %s", script.c_str());
    if (!result.has_value())
    {
        co_return std::unexpected(result.error());
    }
    if (!result->is_string())
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    script_sha_ = result->str;
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::execute_lua(std::string_view cur_key, std::string_view prev_key, uint32_t limit, uint64_t now_ms)
{
    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(config_.window_size).count();

    // Convert all arguments to strings for %s binding.
    // hiredis %s creates proper RESP bulk strings, preventing whitespace splitting
    // that breaks multi-line Lua scripts with the deprecated command(string_view).
    auto cur = std::string(cur_key);
    auto prev = std::string(prev_key);
    auto limit_str = std::to_string(limit);
    auto window_str = std::to_string(window_ms);
    auto now_str = std::to_string(now_ms);

    apex::core::Result<adapters::redis::RedisReply> result;
    if (!script_sha_.empty())
    {
        result = co_await multiplexer_.command("EVALSHA %s 2 %s %s %s %s %s", script_sha_.c_str(), cur.c_str(),
                                               prev.c_str(), limit_str.c_str(), window_str.c_str(), now_str.c_str());
    }
    else
    {
        auto script = std::string(LUA_SCRIPT);
        result = co_await multiplexer_.command("EVAL %s 2 %s %s %s %s %s", script.c_str(), cur.c_str(), prev.c_str(),
                                               limit_str.c_str(), window_str.c_str(), now_str.c_str());
    }

    if (!result.has_value())
    {
        co_return std::unexpected(result.error());
    }

    if (!result->is_array() || result->array.size() < 3)
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto& arr = result->array;
    co_return RateLimitResult{
        .allowed = (arr[0].integer != 0),
        .estimated_count = static_cast<uint32_t>(arr[1].integer),
        .retry_after_ms = static_cast<uint32_t>(arr[2].integer),
    };
}

} // namespace apex::shared::rate_limit
