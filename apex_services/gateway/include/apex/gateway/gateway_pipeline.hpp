#pragma once

#include <apex/gateway/jwt_verifier.hpp>
#include <apex/gateway/jwt_blacklist.hpp>
#include <apex/core/session.hpp>
#include <apex/core/result.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/awaitable.hpp>

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
/// Processing order: JWT verification -> [Redis blacklist] -> routing
class GatewayPipeline {
public:
    GatewayPipeline(const JwtVerifier& jwt_verifier,
                    JwtBlacklist* blacklist);  // nullable -- no Redis connection

    /// Authentication check (called for every message).
    /// Login requests (system msg_id range) skip authentication.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>>
    authenticate(apex::core::SessionPtr session,
                 const apex::core::WireHeader& header,
                 AuthState& state);

private:
    const JwtVerifier& jwt_verifier_;
    JwtBlacklist* blacklist_;
};

} // namespace apex::gateway
