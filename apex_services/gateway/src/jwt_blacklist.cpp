// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/jwt_blacklist.hpp>

#include <algorithm>
#include <format>
#include <string>

namespace apex::gateway
{

namespace detail
{

bool is_valid_jti(std::string_view jti) noexcept
{
    if (jti.empty() || jti.size() > 128)
        return false;
    return std::ranges::all_of(jti, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-';
    });
}

} // namespace detail

JwtBlacklist::JwtBlacklist(apex::shared::adapters::redis::RedisMultiplexer& redis)
    : redis_(redis)
{}

boost::asio::awaitable<apex::core::Result<bool>> JwtBlacklist::is_blacklisted(std::string_view jti)
{
    if (!detail::is_valid_jti(jti))
    {
        logger_.warn("JWT blacklist: invalid jti format, rejecting");
        co_return true; // Reject invalid JTI
    }

    // Build key with safe prefix + parameterized JTI via hiredis escaping
    auto key = std::format("jwt:blacklist:{}", jti);
    auto result = co_await redis_.command("EXISTS %s", key.c_str());
    if (!result)
    {
        logger_.warn("JWT blacklist Redis check failed for jti={}", jti);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    co_return result->integer > 0;
}

} // namespace apex::gateway
