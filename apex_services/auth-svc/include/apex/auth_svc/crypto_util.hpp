#pragma once

#include <openssl/sha.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace apex::auth_svc {

/// SHA-256 hash -> hex string (64 chars)
[[nodiscard]] inline std::string sha256_hex(std::string_view input) {
    std::array<uint8_t, SHA256_DIGEST_LENGTH> hash{};
    SHA256(reinterpret_cast<const uint8_t*>(input.data()),
           input.size(),
           hash.data());

    std::string result;
    result.reserve(SHA256_DIGEST_LENGTH * 2);
    for (auto byte : hash) {
        result += std::format("{:02x}", byte);
    }
    return result;
}

/// Cryptographically secure random token generation (hex, 64 chars default)
[[nodiscard]] inline std::string generate_secure_token(size_t bytes = 32) {
    std::vector<uint8_t> buf(bytes);
    RAND_bytes(buf.data(), static_cast<int>(bytes));

    std::string result;
    result.reserve(bytes * 2);
    for (auto byte : buf) {
        result += std::format("{:02x}", byte);
    }
    return result;
}

} // namespace apex::auth_svc
