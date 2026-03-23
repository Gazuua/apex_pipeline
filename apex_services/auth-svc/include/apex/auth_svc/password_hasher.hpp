// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/scoped_logger.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace apex::auth_svc
{

/// bcrypt-based password hashing/verification.
/// Thread-safe (stateless except work_factor).
class PasswordHasher
{
  public:
    explicit PasswordHasher(uint32_t work_factor = 12);

    /// Hash password with bcrypt
    [[nodiscard]] std::string hash(std::string_view password) const;

    /// Verify password against stored hash
    [[nodiscard]] bool verify(std::string_view password, std::string_view hash) const;

  private:
    uint32_t work_factor_;
    apex::core::ScopedLogger logger_{"PasswordHasher", apex::core::ScopedLogger::NO_CORE, "app"};
};

} // namespace apex::auth_svc
