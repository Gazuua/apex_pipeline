// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/shared/secure_string.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace apex::auth_svc
{

/// JWT token creation/verification manager.
/// Uses RS256 algorithm (asymmetric key -- Auth issues, Gateway can verify).
///
/// Thread-safe: RSA key loading done once in constructor, read-only afterwards.
class JwtManager
{
  public:
    struct Claims
    {
        uint64_t user_id{0};
        std::string email;
        std::string jti;
        std::chrono::system_clock::time_point issued_at;
        std::chrono::system_clock::time_point expires_at;
    };

    /// @param private_key_path RS256 PEM private key path
    /// @param public_key_path RS256 PEM public key path
    /// @param issuer JWT issuer field
    /// @param access_token_ttl Access Token lifetime
    JwtManager(std::string_view private_key_path, std::string_view public_key_path, std::string_view issuer,
               std::chrono::seconds access_token_ttl);

    /// Create Access Token
    [[nodiscard]] std::string create_access_token(uint64_t user_id, std::string_view email) const;

    /// Verify Access Token and return Claims
    [[nodiscard]] apex::core::Result<Claims> verify_access_token(std::string_view token) const;

    /// Extract remaining TTL from token (for blacklist TTL calculation)
    [[nodiscard]] std::chrono::seconds remaining_ttl(std::string_view token) const;

    /// Access Token TTL getter
    [[nodiscard]] std::chrono::seconds access_token_ttl() const noexcept
    {
        return access_token_ttl_;
    }

  private:
    apex::shared::SecureString private_key_; // PEM string (loaded from file, zeroized on destruction)
    std::string public_key_;                 // PEM string (public — no zeroization needed)
    std::string issuer_;
    std::chrono::seconds access_token_ttl_;
    apex::core::ScopedLogger logger_{"JwtManager", apex::core::ScopedLogger::NO_CORE, "app"};
};

} // namespace apex::auth_svc
