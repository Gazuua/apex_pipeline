// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/password_hasher.hpp>

// Bundled bcrypt implementation (OpenSSL-based, no external dependency)
extern "C"
{
#include "bcrypt/apex_bcrypt.h"
}

namespace apex::auth_svc
{

PasswordHasher::PasswordHasher(uint32_t work_factor)
    : work_factor_(work_factor)
{}

std::string PasswordHasher::hash(std::string_view password) const
{
    char salt[BCRYPT_HASHSIZE];
    char hash_buf[BCRYPT_HASHSIZE];

    if (apex_bcrypt_gensalt(static_cast<int>(work_factor_), salt) != 0)
    {
        logger_.error("bcrypt salt generation failed");
        return {};
    }

    if (apex_bcrypt_hashpw(std::string(password).c_str(), salt, hash_buf) != 0)
    {
        logger_.error("bcrypt hash failed");
        return {};
    }

    return std::string(hash_buf);
}

bool PasswordHasher::verify(std::string_view password, std::string_view stored_hash) const
{
    // apex_bcrypt_checkpw: 0 = match
    logger_.trace("verify: hash_len={}", stored_hash.size());
    int ret = apex_bcrypt_checkpw(std::string(password).c_str(), std::string(stored_hash).c_str());
    logger_.trace("verify: result={}", ret == 0 ? "match" : "mismatch");
    return ret == 0;
}

} // namespace apex::auth_svc
