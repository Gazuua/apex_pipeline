#include <apex/shared/rate_limit/redis_rate_limiter.hpp>

#include <format>

namespace apex::shared::rate_limit {

RedisRateLimiter::RedisRateLimiter(
    RedisRateLimiterConfig config,
    adapters::redis::RedisMultiplexer& multiplexer)
    : config_(config), multiplexer_(multiplexer) {}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::check_user(uint64_t user_id, uint64_t now_ms) {
    auto cur_key = std::format("rl:user:{}:cur", user_id);
    auto prev_key = std::format("rl:user:{}:prev", user_id);
    co_return co_await execute_lua(cur_key, prev_key, config_.default_limit, now_ms);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::check_endpoint(uint64_t user_id, uint32_t msg_id,
                                 uint64_t now_ms, uint32_t limit_override) {
    auto cur_key = std::format("rl:ep:{}:{}:cur", user_id, msg_id);
    auto prev_key = std::format("rl:ep:{}:{}:prev", user_id, msg_id);
    auto limit = (limit_override > 0) ? limit_override : config_.default_limit;
    co_return co_await execute_lua(cur_key, prev_key, limit, now_ms);
}

void RedisRateLimiter::update_config(RedisRateLimiterConfig config) noexcept {
    config_ = config;
}

boost::asio::awaitable<apex::core::Result<void>>
RedisRateLimiter::load_script() {
    auto cmd = std::format("SCRIPT LOAD {}", LUA_SCRIPT);
    auto result = co_await multiplexer_.command(cmd);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }
    if (!result->is_string()) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    script_sha_ = result->str;
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::execute_lua(std::string_view cur_key, std::string_view prev_key,
                              uint32_t limit, uint64_t now_ms) {
    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        config_.window_size).count();

    std::string cmd;
    if (!script_sha_.empty()) {
        cmd = std::format("EVALSHA {} 2 {} {} {} {} {}",
                          script_sha_, cur_key, prev_key,
                          limit, window_ms, now_ms);
    } else {
        // Fallback to EVAL if script not loaded
        cmd = std::format("EVAL \"{}\" 2 {} {} {} {} {}",
                          LUA_SCRIPT, cur_key, prev_key,
                          limit, window_ms, now_ms);
    }

    auto result = co_await multiplexer_.command(cmd);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }

    if (!result->is_array() || result->array.size() < 3) {
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
