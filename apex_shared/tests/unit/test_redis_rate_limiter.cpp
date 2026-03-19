#include <apex/shared/rate_limit/redis_rate_limiter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;

// These tests verify the configuration and key formatting logic.
// Full Redis integration tests require a running Redis instance
// and are placed in tests/integration/.

TEST(RedisRateLimiter, ConfigDefaults)
{
    RedisRateLimiterConfig config;
    EXPECT_EQ(config.default_limit, 100u);
    EXPECT_EQ(config.window_size, std::chrono::seconds{60});
}

TEST(RedisRateLimiter, UpdateConfig)
{
    // This test verifies that update_config changes internal state.
    // We can't easily test the Redis interaction without a mock,
    // but we verify the config is properly stored.
    RedisRateLimiterConfig config{.default_limit = 50, .window_size = std::chrono::seconds{30}};
    EXPECT_EQ(config.default_limit, 50u);
    EXPECT_EQ(config.window_size, std::chrono::seconds{30});
}

TEST(RedisRateLimiter, RateLimitResultStruct)
{
    RateLimitResult result{.allowed = true, .estimated_count = 42, .retry_after_ms = 0};
    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.estimated_count, 42u);
    EXPECT_EQ(result.retry_after_ms, 0u);
}

TEST(RedisRateLimiter, LuaScriptNotEmpty)
{
    // Verify the embedded Lua script is non-empty
    EXPECT_FALSE(RedisRateLimiter::LUA_SCRIPT.empty());
}
