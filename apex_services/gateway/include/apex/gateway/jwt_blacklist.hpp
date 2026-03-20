// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <string_view>

namespace apex::gateway
{

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
