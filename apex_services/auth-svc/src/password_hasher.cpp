#include <apex/auth_svc/password_hasher.hpp>

#include <spdlog/spdlog.h>

// Bundled bcrypt implementation (OpenSSL-based, no external dependency)
extern "C" {
#include "bcrypt/bcrypt.h"
}

namespace apex::auth_svc {

PasswordHasher::PasswordHasher(uint32_t work_factor)
    : work_factor_(work_factor)
{}

std::string PasswordHasher::hash(std::string_view password) const {
    char salt[BCRYPT_HASHSIZE];
    char hash_buf[BCRYPT_HASHSIZE];

    if (apex_bcrypt_gensalt(static_cast<int>(work_factor_), salt) != 0) {
        spdlog::error("[PasswordHasher] bcrypt salt generation failed");
        return {};
    }

    if (apex_bcrypt_hashpw(std::string(password).c_str(), salt, hash_buf) != 0) {
        spdlog::error("[PasswordHasher] bcrypt hash failed");
        return {};
    }

    return std::string(hash_buf);
}

bool PasswordHasher::verify(std::string_view password,
                            std::string_view stored_hash) const {
    // apex_bcrypt_checkpw: 0 = match
    int ret = apex_bcrypt_checkpw(std::string(password).c_str(),
                                   std::string(stored_hash).c_str());
    return ret == 0;
}

} // namespace apex::auth_svc
