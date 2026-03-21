// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <string_view>

namespace apex::gateway
{

namespace detail
{

/// Validate JTI contains only safe characters (hex digits + hyphens, max 128 chars).
/// Prevents Redis command injection via crafted JTI values.
[[nodiscard]] bool is_valid_jti(std::string_view jti) noexcept;

} // namespace detail

/// JWT blacklist Redis check (cold path).
/// Called only for sensitive msg_ids. Redis key: "jwt:blacklist:{jti}"
class JwtBlacklist
{
  public:
    explicit JwtBlacklist(apex::shared::adapters::redis::RedisMultiplexer& redis);

    /// Check if JTI (JWT ID) is blacklisted.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<bool>> is_blacklisted(std::string_view jti);

  private:
    apex::shared::adapters::redis::RedisMultiplexer& redis_;
};

} // namespace apex::gateway
