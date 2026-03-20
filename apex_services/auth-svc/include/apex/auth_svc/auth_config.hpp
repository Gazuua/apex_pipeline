// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace apex::auth_svc
{

struct AuthConfig
{
    // JWT
    std::string jwt_private_key_path = "keys/auth_rs256.pem";
    std::string jwt_public_key_path = "keys/auth_rs256_pub.pem";
    std::string jwt_issuer = "apex-auth";
    std::chrono::seconds access_token_ttl{900};     // 15min
    std::chrono::seconds refresh_token_ttl{604800}; // 7 days

    // Kafka
    std::string request_topic = "auth.requests";
    std::string response_topic = "auth.responses";

    // Redis (auth dedicated -- Redis #0)
    std::string redis_session_prefix = "auth:session:";
    std::string redis_blacklist_prefix = "jwt:blacklist:";

    // bcrypt
    uint32_t bcrypt_work_factor = 12;

    // Session
    std::chrono::seconds session_ttl{86400}; // 24h
};

} // namespace apex::auth_svc
