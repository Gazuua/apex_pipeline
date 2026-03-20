// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace apex::auth_svc
{

/// Redis-based session store.
///
/// Key format: `auth:session:{user_id}`
/// Value: simple `"uid:{uid}|email:{email}|created:{ts}"` format
///
/// TTL: session validity period. SET + EXPIRE on login, DEL on logout.
class SessionStore
{
  public:
    SessionStore(apex::shared::adapters::redis::RedisAdapter& redis, std::string prefix, std::string blacklist_prefix,
                 std::chrono::seconds ttl);

    /// Create/update session
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> set(uint64_t user_id, std::string_view session_data);

    /// Lookup session
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<std::string>> get(uint64_t user_id);

    /// Delete session (logout)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> remove(uint64_t user_id);

    /// Store session:user:{uid} -> session_id mapping (for cross-service session lookup)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> set_user_session_id(uint64_t user_id,
                                                                                       uint64_t session_id);

    /// Remove session:user:{uid} mapping
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> remove_user_session_id(uint64_t user_id);

    /// Add JWT to blacklist (token hash based)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> blacklist_token(std::string_view token_hash,
                                                                                   std::chrono::seconds ttl);

    /// Check if JWT is blacklisted
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<bool>> is_blacklisted(std::string_view token_hash);

  private:
    [[nodiscard]] std::string session_key(uint64_t user_id) const;
    [[nodiscard]] std::string blacklist_key(std::string_view token_hash) const;

    apex::shared::adapters::redis::RedisAdapter& redis_;
    std::string prefix_;
    std::string blacklist_prefix_;
    std::chrono::seconds ttl_;
};

} // namespace apex::auth_svc
