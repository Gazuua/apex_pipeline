// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/auth_error.hpp>
#include <apex/auth_svc/crypto_util.hpp>
#include <apex/auth_svc/jwt_manager.hpp>

#include <apex/core/scoped_logger.hpp>

#include <jwt-cpp/jwt.h>

#include <charconv>
#include <fstream>
#include <sstream>

namespace apex::auth_svc
{

namespace
{

apex::core::ScopedLogger s_logger{"JwtManager", apex::core::ScopedLogger::NO_CORE, "app"};

std::string read_file(std::string_view path)
{
    std::ifstream file{std::string{path}};
    if (!file.is_open())
    {
        s_logger.error("Failed to open key file: {}", path);
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// Generate a cryptographically secure random hex string for JWT ID (jti).
/// 16 bytes = 32 hex chars via RAND_bytes (CSPRNG).
std::string generate_jti()
{
    auto result = generate_secure_token(16);
    if (!result.has_value())
    {
        s_logger.error("CSPRNG failure in generate_jti");
        return {};
    }
    return std::move(*result);
}

} // anonymous namespace

JwtManager::JwtManager(std::string_view private_key_path, std::string_view public_key_path, std::string_view issuer,
                       std::chrono::seconds access_token_ttl)
    : private_key_(read_file(private_key_path))
    , public_key_(read_file(public_key_path))
    , issuer_(issuer)
    , access_token_ttl_(access_token_ttl)
{
    if (private_key_.empty())
    {
        logger_.warn("Private key not loaded -- token creation disabled");
    }
    if (public_key_.empty())
    {
        logger_.warn("Public key not loaded -- token verification disabled");
    }
}

std::string JwtManager::create_access_token(uint64_t user_id, std::string_view email) const
{
    if (private_key_.empty())
    {
        logger_.error("Cannot create token -- private key not loaded");
        return {};
    }

    try
    {
        auto now = std::chrono::system_clock::now();
        auto exp = now + access_token_ttl_;

        auto jti = generate_jti();
        if (jti.empty())
        {
            logger_.error("Cannot create token -- JTI generation failed (CSPRNG)");
            return {};
        }

        auto token = jwt::create()
                         .set_issuer(issuer_)
                         .set_type("JWT")
                         .set_issued_at(now)
                         .set_expires_at(exp)
                         .set_payload_claim("uid", jwt::claim(std::to_string(user_id)))
                         .set_subject(std::string(email))
                         .set_payload_claim("jti", jwt::claim(std::string(jti)))
                         .sign(jwt::algorithm::rs256(public_key_, private_key_));

        return token;
    }
    catch (const std::exception& e)
    {
        logger_.error("Token creation failed: {}", e.what());
        return {};
    }
}

apex::core::Result<JwtManager::Claims> JwtManager::verify_access_token(std::string_view token) const
{
    if (public_key_.empty())
    {
        return apex::core::error(apex::core::ErrorCode::AdapterError);
    }

    try
    {
        auto verifier = jwt::verify().allow_algorithm(jwt::algorithm::rs256(public_key_)).with_issuer(issuer_);

        auto decoded = jwt::decode(std::string(token));
        verifier.verify(decoded);

        Claims claims;
        {
            auto uid_str = decoded.get_payload_claim("uid").as_string();
            auto [ptr, ec] = std::from_chars(uid_str.data(), uid_str.data() + uid_str.size(), claims.user_id);
            if (ec != std::errc{})
            {
                logger_.debug("uid claim parsing failed: '{}'", uid_str);
                return apex::core::error(apex::core::ErrorCode::ServiceError);
            }
        }
        claims.email = decoded.get_subject();
        if (decoded.has_payload_claim("jti"))
        {
            claims.jti = decoded.get_payload_claim("jti").as_string();
        }
        claims.issued_at = decoded.get_issued_at();
        claims.expires_at = decoded.get_expires_at();

        return claims;
    }
    catch (const jwt::error::token_verification_exception& e)
    {
        logger_.debug("Token verification failed: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::ServiceError);
    }
    catch (const std::exception& e)
    {
        logger_.error("Unexpected error: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::AdapterError);
    }
}

std::chrono::seconds JwtManager::remaining_ttl(std::string_view token) const
{
    try
    {
        auto decoded = jwt::decode(std::string(token));
        auto exp = decoded.get_expires_at();
        auto now = std::chrono::system_clock::now();

        if (exp <= now)
        {
            return std::chrono::seconds{0};
        }

        return std::chrono::duration_cast<std::chrono::seconds>(exp - now);
    }
    catch (...)
    {
        return std::chrono::seconds{0};
    }
}

} // namespace apex::auth_svc
