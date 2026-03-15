#include <apex/auth_svc/jwt_manager.hpp>

#include <jwt-cpp/jwt.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace apex::auth_svc {

namespace {

std::string read_file(std::string_view path) {
    std::ifstream file(std::string(path));
    if (!file.is_open()) {
        spdlog::error("[JwtManager] Failed to open key file: {}", path);
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // anonymous namespace

JwtManager::JwtManager(std::string_view private_key_path,
                       std::string_view public_key_path,
                       std::string_view issuer,
                       std::chrono::seconds access_token_ttl)
    : private_key_(read_file(private_key_path))
    , public_key_(read_file(public_key_path))
    , issuer_(issuer)
    , access_token_ttl_(access_token_ttl)
{
    if (private_key_.empty()) {
        spdlog::warn("[JwtManager] Private key not loaded -- token creation disabled");
    }
    if (public_key_.empty()) {
        spdlog::warn("[JwtManager] Public key not loaded -- token verification disabled");
    }
}

std::string JwtManager::create_access_token(uint64_t user_id,
                                             std::string_view email) const {
    if (private_key_.empty()) {
        spdlog::error("[JwtManager] Cannot create token -- private key not loaded");
        return {};
    }

    try {
        auto now = std::chrono::system_clock::now();
        auto exp = now + access_token_ttl_;

        auto token = jwt::create()
            .set_issuer(issuer_)
            .set_type("JWT")
            .set_issued_at(now)
            .set_expires_at(exp)
            .set_payload_claim("uid", jwt::claim(std::to_string(user_id)))
            .set_payload_claim("email", jwt::claim(std::string(email)))
            .sign(jwt::algorithm::rs256(public_key_, private_key_));

        return token;
    } catch (const std::exception& e) {
        spdlog::error("[JwtManager] Token creation failed: {}", e.what());
        return {};
    }
}

apex::core::Result<JwtManager::Claims> JwtManager::verify_access_token(
    std::string_view token) const
{
    if (public_key_.empty()) {
        return apex::core::error(apex::core::ErrorCode::AdapterError);
    }

    try {
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::rs256(public_key_, private_key_))
            .with_issuer(issuer_);

        auto decoded = jwt::decode(std::string(token));
        verifier.verify(decoded);

        Claims claims;
        claims.user_id = std::stoull(decoded.get_payload_claim("uid").as_string());
        claims.email = decoded.get_payload_claim("email").as_string();
        claims.issued_at = decoded.get_issued_at();
        claims.expires_at = decoded.get_expires_at();

        return claims;

    } catch (const jwt::error::token_verification_exception& e) {
        spdlog::debug("[JwtManager] Token verification failed: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::JwtVerifyFailed);
    } catch (const std::exception& e) {
        spdlog::error("[JwtManager] Unexpected error: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::AdapterError);
    }
}

std::chrono::seconds JwtManager::remaining_ttl(std::string_view token) const {
    try {
        auto decoded = jwt::decode(std::string(token));
        auto exp = decoded.get_expires_at();
        auto now = std::chrono::system_clock::now();

        if (exp <= now) {
            return std::chrono::seconds{0};
        }

        return std::chrono::duration_cast<std::chrono::seconds>(exp - now);
    } catch (...) {
        return std::chrono::seconds{0};
    }
}

} // namespace apex::auth_svc
