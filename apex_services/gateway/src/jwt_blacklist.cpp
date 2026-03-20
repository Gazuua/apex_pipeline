// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/jwt_blacklist.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <string>

namespace apex::gateway
{

namespace
{

/// Validate JTI contains only safe characters (alphanumeric + hyphens).
/// Prevents Redis command injection via crafted JTI values.
bool is_valid_jti(std::string_view jti) noexcept
{
    if (jti.empty() || jti.size() > 128)
        return false;
    return std::ranges::all_of(jti, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-';
    });
}

} // anonymous namespace

JwtBlacklist::JwtBlacklist(apex::shared::adapters::redis::RedisMultiplexer& redis)
    : redis_(redis)
{}

boost::asio::awaitable<apex::core::Result<bool>> JwtBlacklist::is_blacklisted(std::string_view jti)
{
    if (!is_valid_jti(jti))
    {
        spdlog::warn("JWT blacklist: invalid jti format, rejecting");
        co_return true; // Reject invalid JTI
    }

    // Build key with safe prefix + parameterized JTI via hiredis escaping
    auto key = std::format("jwt:blacklist:{}", jti);
    auto result = co_await redis_.command("EXISTS %s", key.c_str());
    if (!result)
    {
        // Redis failure: fail-open (availability priority).
        // Conservative: treat as blacklisted for sensitive path.
        spdlog::warn("JWT blacklist check failed for jti={}, assuming blacklisted", jti);
        co_return true;
    }
    co_return result->integer > 0;
}

} // namespace apex::gateway
