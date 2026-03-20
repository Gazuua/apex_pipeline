// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

namespace apex::auth_svc
{

enum class AuthError : uint16_t
{
    JwtVerifyFailed = 1,
};

constexpr std::string_view auth_error_name(AuthError code) noexcept
{
    switch (code)
    {
        case AuthError::JwtVerifyFailed:
            return "JwtVerifyFailed";
        default:
            return "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, AuthError ec)
{
    return os << auth_error_name(ec);
}

} // namespace apex::auth_svc
