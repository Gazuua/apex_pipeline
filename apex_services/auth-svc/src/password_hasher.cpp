#include <apex/auth_svc/password_hasher.hpp>

#include <spdlog/spdlog.h>

// vcpkg libbcrypt provides C++ API via BCrypt.hpp
#include <bcrypt/BCrypt.hpp>

namespace apex::auth_svc {

PasswordHasher::PasswordHasher(uint32_t work_factor)
    : work_factor_(work_factor)
{}

std::string PasswordHasher::hash(std::string_view password) const {
    try {
        return BCrypt::generateHash(std::string(password),
                                     static_cast<int>(work_factor_));
    } catch (const std::exception& e) {
        spdlog::error("[PasswordHasher] Hash generation failed: {}", e.what());
        return {};
    }
}

bool PasswordHasher::verify(std::string_view password,
                            std::string_view stored_hash) const {
    try {
        return BCrypt::validatePassword(std::string(password),
                                         std::string(stored_hash));
    } catch (const std::exception& e) {
        spdlog::error("[PasswordHasher] Verification failed: {}", e.what());
        return false;
    }
}

} // namespace apex::auth_svc
