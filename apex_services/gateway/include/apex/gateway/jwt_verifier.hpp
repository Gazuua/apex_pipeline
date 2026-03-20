// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/gateway/gateway_config.hpp>

#include <jwt-cpp/jwt.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace apex::gateway
{

/// JWT verification result.
struct JwtClaims
{
    uint64_t user_id{0};
    std::string email; // Auth service sets sub = email
    std::string jti;   // JWT ID (for blacklist check)
    std::chrono::system_clock::time_point expires_at;
};

/// JWT signature verifier (RS256).
/// Local signature verification only (hot path, zero network cost).
/// Sensitive msg_ids require separate Redis blacklist check.
class JwtVerifier
{
  public:
    explicit JwtVerifier(const JwtConfig& config);

    JwtVerifier(const JwtVerifier&) = delete;
    JwtVerifier& operator=(const JwtVerifier&) = delete;
    JwtVerifier(JwtVerifier&&) = delete;
    JwtVerifier& operator=(JwtVerifier&&) = delete;

    /// Verify JWT token.
    /// @param token Pure token string without "Bearer " prefix
    /// @return Claims or error
    [[nodiscard]] apex::core::Result<JwtClaims> verify(std::string_view token) const;

    /// Check if msg_id is sensitive (requires Redis blacklist check).
    [[nodiscard]] bool is_sensitive(uint32_t msg_id) const noexcept;

  private:
    JwtConfig config_;
    std::string public_key_; // PEM public key (loaded from file)
    decltype(jwt::verify()) verifier_;
};

} // namespace apex::gateway
