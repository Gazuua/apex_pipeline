// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/jwt_verifier.hpp>

#include <apex/gateway/gateway_error.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace apex::gateway
{

namespace
{

std::string read_file(std::string_view path)
{
    std::ifstream file{std::string{path}};
    if (!file.is_open())
    {
        spdlog::error("[JwtVerifier] Failed to open key file: {}", path);
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // anonymous namespace

JwtVerifier::JwtVerifier(const JwtConfig& config)
    : config_(config)
    , public_key_(read_file(config.public_key_file))
    , verifier_(jwt::verify())
{
    if (public_key_.empty())
    {
        spdlog::error("[JwtVerifier] RS256 public key not loaded from '{}' "
                      "-- token verification will fail",
                      config.public_key_file);
    }
    else
    {
        verifier_.allow_algorithm(jwt::algorithm::rs256(public_key_));
    }
    verifier_.with_issuer(config.issuer);
    verifier_.leeway(static_cast<uint64_t>(config.clock_skew.count()));
    if (config.issuer.empty())
    {
        spdlog::error("[JwtVerifier] JWT issuer is empty "
                      "-- token issuer validation is effectively disabled");
    }
}

apex::core::Result<JwtClaims> JwtVerifier::verify(std::string_view token) const
{
    try
    {
        auto decoded = jwt::decode(std::string(token));
        verifier_.verify(decoded);

        JwtClaims claims;
        // Auth service stores uid as string (avoids double precision loss for large IDs)
        claims.user_id = std::stoull(decoded.get_payload_claim("uid").as_string());
        // Auth service sets sub = email
        claims.email = decoded.get_subject();
        if (decoded.has_payload_claim("jti"))
        {
            claims.jti = decoded.get_payload_claim("jti").as_string();
        }
        claims.expires_at = decoded.get_expires_at();

        return claims;
    }
    catch (const jwt::error::token_verification_exception& e)
    {
        spdlog::debug("JWT verification failed: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::ServiceError);
    }
    catch (const jwt::error::claim_not_present_exception& e)
    {
        spdlog::debug("JWT missing claim: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::ServiceError);
    }
    catch (const std::exception& e)
    {
        spdlog::warn("JWT unexpected error: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::ServiceError);
    }
}

bool JwtVerifier::is_sensitive(uint32_t msg_id) const noexcept
{
    // Linear search -- sensitive_msg_ids is typically < 10 elements
    return std::ranges::find(config_.sensitive_msg_ids, msg_id) != config_.sensitive_msg_ids.end();
}

} // namespace apex::gateway
