// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <expected>
#include <ostream>
#include <string_view>

namespace apex::gateway
{

enum class GatewayError : uint16_t
{
    ConfigParseFailed = 1,
    JwtVerifyFailed = 2,
    JwtExpired = 3,
    JwtBlacklisted = 4,
    RouteNotFound = 5,
    ServiceTimeout = 6,
    PendingMapFull = 7,
    RateLimitedIp = 8,
    RateLimitedUser = 9,
    RateLimitedEndpoint = 10,
    SubscriptionLimitExceeded = 11,
    BlacklistCheckFailed = 12,
};

constexpr std::string_view gateway_error_name(GatewayError code) noexcept
{
    switch (code)
    {
        case GatewayError::ConfigParseFailed:
            return "ConfigParseFailed";
        case GatewayError::JwtVerifyFailed:
            return "JwtVerifyFailed";
        case GatewayError::JwtExpired:
            return "JwtExpired";
        case GatewayError::JwtBlacklisted:
            return "JwtBlacklisted";
        case GatewayError::RouteNotFound:
            return "RouteNotFound";
        case GatewayError::ServiceTimeout:
            return "ServiceTimeout";
        case GatewayError::PendingMapFull:
            return "PendingMapFull";
        case GatewayError::RateLimitedIp:
            return "RateLimitedIp";
        case GatewayError::RateLimitedUser:
            return "RateLimitedUser";
        case GatewayError::RateLimitedEndpoint:
            return "RateLimitedEndpoint";
        case GatewayError::SubscriptionLimitExceeded:
            return "SubscriptionLimitExceeded";
        case GatewayError::BlacklistCheckFailed:
            return "BlacklistCheckFailed";
        default:
            return "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, GatewayError ec)
{
    return os << gateway_error_name(ec);
}

/// Gateway-internal result type for pipeline sub-functions.
using GatewayResult = std::expected<void, GatewayError>;

} // namespace apex::gateway
