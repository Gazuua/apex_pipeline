#include <apex/gateway/jwt_blacklist.hpp>

#include <spdlog/spdlog.h>

#include <format>

namespace apex::gateway {

JwtBlacklist::JwtBlacklist(
    apex::shared::adapters::redis::RedisMultiplexer& redis)
    : redis_(redis) {}

boost::asio::awaitable<apex::core::Result<bool>>
JwtBlacklist::is_blacklisted(std::string_view jti) {
    auto cmd = std::format("EXISTS jwt:blacklist:{}", jti);
    auto result = co_await redis_.command(cmd);
    if (!result) {
        // Redis failure: fail-open (availability priority).
        // Conservative: treat as blacklisted for sensitive path.
        spdlog::warn("JWT blacklist check failed for jti={}, assuming blacklisted", jti);
        co_return true;
    }
    co_return result->as_integer() > 0;
}

} // namespace apex::gateway
