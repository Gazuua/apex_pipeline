#pragma once

#include <apex/gateway/jwt_verifier.hpp>
#include <apex/gateway/jwt_blacklist.hpp>
#include <apex/shared/rate_limit/rate_limit_facade.hpp>
#include <apex/core/session.hpp>
#include <apex/core/result.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>

namespace apex::gateway {

/// Per-session authentication state.
/// Attached to Session as user_data or managed in separate map.
struct AuthState {
    bool authenticated = false;
    uint64_t user_id = 0;
    std::string jti;  // JWT ID (for blacklist check)
};

/// Gateway request pipeline.
/// Processing order:
///   [Per-IP rate limit] -> JWT verification -> [Redis blacklist]
///   -> [Per-User rate limit] -> [Per-Endpoint rate limit] -> routing
class GatewayPipeline {
public:
    /// @param jwt_verifier JWT signature verifier.
    /// @param blacklist JWT blacklist checker (nullable -- no Redis connection).
    /// @param rate_limiter 3-layer rate limit facade (nullable -- rate limiting disabled).
    GatewayPipeline(const JwtVerifier& jwt_verifier,
                    JwtBlacklist* blacklist,
                    apex::shared::rate_limit::RateLimitFacade* rate_limiter = nullptr);

    /// Full pipeline check for a request.
    /// Runs rate limiting (Per-IP, Per-User, Per-Endpoint) and authentication.
    /// @param session Request session.
    /// @param header Client WireHeader.
    /// @param state Per-session auth state (modified on successful auth).
    /// @param remote_ip Client IP address string (caller resolves from socket).
    /// @return Ok if request should proceed, error code otherwise.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>>
    process(apex::core::SessionPtr session,
            const apex::core::WireHeader& header,
            AuthState& state,
            std::string_view remote_ip);

    /// Authentication check only (called for every message).
    /// Login requests (system msg_id range) skip authentication.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>>
    authenticate(apex::core::SessionPtr session,
                 const apex::core::WireHeader& header,
                 AuthState& state);

    /// Per-IP rate limit check (Layer 1, local memory, no I/O).
    /// Called before JWT verification.
    /// @param remote_ip Client IP address.
    /// @return Ok if allowed, RateLimitedIp if denied.
    [[nodiscard]] apex::core::Result<void>
    check_ip_rate_limit(std::string_view remote_ip);

    /// Per-User rate limit check (Layer 2, Redis).
    /// Called after JWT verification.
    /// @param user_id Authenticated user ID.
    /// @return Ok if allowed, RateLimitedUser if denied.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>>
    check_user_rate_limit(uint64_t user_id);

    /// Per-Endpoint rate limit check (Layer 3, Redis).
    /// Called before msg_id routing.
    /// @param user_id Authenticated user ID.
    /// @param msg_id Message type ID.
    /// @return Ok if allowed, RateLimitedEndpoint if denied.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>>
    check_endpoint_rate_limit(uint64_t user_id, uint32_t msg_id);

    /// Update rate limiter reference (for hot-reload / lazy init).
    void set_rate_limiter(apex::shared::rate_limit::RateLimitFacade* limiter) noexcept;

private:
    /// Get current epoch milliseconds.
    [[nodiscard]] static uint64_t now_ms() noexcept;

    const JwtVerifier& jwt_verifier_;
    JwtBlacklist* blacklist_;
    apex::shared::rate_limit::RateLimitFacade* rate_limiter_;
};

} // namespace apex::gateway
