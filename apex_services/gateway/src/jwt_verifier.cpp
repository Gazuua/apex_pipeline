#include <apex/gateway/jwt_verifier.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace apex::gateway {

JwtVerifier::JwtVerifier(const JwtConfig& config)
    : config_(config)
    , verifier_(jwt::verify()
        .allow_algorithm(jwt::algorithm::hs256{config.secret})
        .with_issuer(config.issuer)
        .leeway(static_cast<uint64_t>(config.clock_skew.count()))) {
    if (config.issuer.empty()) {
        spdlog::error("JWT issuer is empty -- token issuer validation is effectively disabled");
    }
}

apex::core::Result<JwtClaims>
JwtVerifier::verify(std::string_view token) const {
    try {
        auto decoded = jwt::decode(std::string(token));
        verifier_.verify(decoded);

        JwtClaims claims;
        claims.user_id = static_cast<uint64_t>(
            decoded.get_payload_claim("uid").as_integer());
        claims.username = decoded.get_payload_claim("sub")
            .as_string();
        if (decoded.has_payload_claim("jti")) {
            claims.jti = decoded.get_payload_claim("jti").as_string();
        }
        claims.expires_at = decoded.get_expires_at();

        return claims;

    } catch (const jwt::error::token_verification_exception& e) {
        spdlog::debug("JWT verification failed: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::JwtVerifyFailed);
    } catch (const jwt::error::claim_not_present_exception& e) {
        spdlog::debug("JWT missing claim: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::JwtVerifyFailed);
    } catch (const std::exception& e) {
        spdlog::warn("JWT unexpected error: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::JwtVerifyFailed);
    }
}

bool JwtVerifier::is_sensitive(uint32_t msg_id) const noexcept {
    // Linear search -- sensitive_msg_ids is typically < 10 elements
    return std::ranges::find(config_.sensitive_msg_ids, msg_id)
        != config_.sensitive_msg_ids.end();
}

} // namespace apex::gateway
