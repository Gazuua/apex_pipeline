// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace apex::shared
{

/// String wrapper that zeroes memory on destruction.
/// Use for password, secret key, and other sensitive string fields.
/// Copies are allowed — each copy is independently zeroed on destruction.
class SecureString
{
  public:
    SecureString() = default;
    ~SecureString();

    SecureString(const SecureString& other);
    SecureString& operator=(const SecureString& other);

    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    SecureString(const char* s);       // NOLINT(google-explicit-constructor)
    SecureString(std::string_view sv); // NOLINT(google-explicit-constructor)
    explicit SecureString(std::string&& s) noexcept;

    [[nodiscard]] const char* c_str() const noexcept;
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] bool operator==(const SecureString& other) const noexcept;
    [[nodiscard]] bool operator==(std::string_view other) const noexcept;
    [[nodiscard]] bool operator==(const char* other) const noexcept;
    [[nodiscard]] bool operator!=(const SecureString& other) const noexcept;
    [[nodiscard]] bool operator!=(std::string_view other) const noexcept;
    [[nodiscard]] bool operator!=(const char* other) const noexcept;

    /// Constant-time comparison — immune to timing side-channel attacks.
    /// Use for security-sensitive comparisons (token validation, password verification, etc.).
    /// operator== remains as fast early-return comparison for non-security contexts.
    [[nodiscard]] bool constant_time_equal(const SecureString& other) const noexcept;
    [[nodiscard]] bool constant_time_equal(std::string_view other) const noexcept;

  private:
    std::string data_;
    void clear() noexcept;
};

} // namespace apex::shared
