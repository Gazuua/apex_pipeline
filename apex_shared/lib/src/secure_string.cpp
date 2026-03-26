// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#include <apex/shared/secure_string.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstring> // explicit_bzero
#endif

namespace apex::shared
{

void SecureString::clear() noexcept
{
    if (!data_.empty())
    {
        auto* ptr = data_.data();
        auto len = data_.size();
#ifdef _WIN32
        SecureZeroMemory(ptr, len);
#else
        explicit_bzero(ptr, len);
#endif
        data_.clear();
        data_.shrink_to_fit();
    }
}

SecureString::~SecureString()
{
    clear();
    // SSO 버퍼 잔류 방지 — clear() 후 string은 empty 상태이므로
    // 전체 바이트 제로화 후 소멸자가 no-op (MSVC/libstdc++/libc++).
#ifdef _WIN32
    SecureZeroMemory(&data_, sizeof(data_));
#else
    explicit_bzero(&data_, sizeof(data_));
#endif
}

SecureString::SecureString(const char* s)
    : data_(s ? s : "")
{}

SecureString::SecureString(std::string_view sv)
    : data_(sv)
{}

SecureString::SecureString(std::string&& s) noexcept
    : data_(std::move(s))
{}

SecureString::SecureString(const SecureString& other)
    : data_(other.data_)
{}

SecureString& SecureString::operator=(const SecureString& other)
{
    if (this != &other)
    {
        clear();
        data_ = other.data_;
    }
    return *this;
}

SecureString::SecureString(SecureString&& other) noexcept
    : data_(std::move(other.data_))
{
    // move 후 소스 SSO 버퍼 잔류 방지
#ifdef _WIN32
    SecureZeroMemory(&other.data_, sizeof(other.data_));
#else
    explicit_bzero(&other.data_, sizeof(other.data_));
#endif
}

SecureString& SecureString::operator=(SecureString&& other) noexcept
{
    if (this != &other)
    {
        clear();
        data_ = std::move(other.data_);
        // move 후 소스 SSO 버퍼 잔류 방지
#ifdef _WIN32
        SecureZeroMemory(&other.data_, sizeof(other.data_));
#else
        explicit_bzero(&other.data_, sizeof(other.data_));
#endif
    }
    return *this;
}

const char* SecureString::c_str() const noexcept
{
    return data_.c_str();
}
std::string_view SecureString::view() const noexcept
{
    return data_;
}
bool SecureString::empty() const noexcept
{
    return data_.empty();
}
std::size_t SecureString::size() const noexcept
{
    return data_.size();
}

bool SecureString::operator==(const SecureString& other) const noexcept
{
    return constant_time_equal(other.view());
}
bool SecureString::operator==(std::string_view other) const noexcept
{
    return constant_time_equal(other);
}
bool SecureString::operator==(const char* other) const noexcept
{
    return other ? constant_time_equal(std::string_view{other}) : data_.empty();
}
bool SecureString::operator!=(const SecureString& other) const noexcept
{
    return !constant_time_equal(other.view());
}
bool SecureString::operator!=(std::string_view other) const noexcept
{
    return !constant_time_equal(other);
}
bool SecureString::operator!=(const char* other) const noexcept
{
    return !(*this == other);
}

bool SecureString::constant_time_equal(const SecureString& other) const noexcept
{
    return constant_time_equal(other.view());
}

bool SecureString::constant_time_equal(std::string_view other) const noexcept
{
    if (data_.size() != other.size())
    {
        return false;
    }
    volatile unsigned char acc = 0;
    for (std::size_t i = 0; i < data_.size(); ++i)
    {
        acc |= static_cast<unsigned char>(data_[i]) ^ static_cast<unsigned char>(other[i]);
    }
    return acc == 0;
}

} // namespace apex::shared
