// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <array>
#include <chrono>
#include <ctime>
#include <string_view>

namespace apex::auth_svc
{

/// 계정 잠금 여부 판정 (순수 함수).
/// @param locked_until_str  PostgreSQL timestamptz 문자열 (e.g., "2026-03-22 12:00:00+00").
///                          ISO 8601 정렬이 가능한 형식이어야 한다.
/// @param now               비교 기준 시점 (UTC).
/// @return true = 현재 잠금 중, false = 잠금 해제됨 또는 만료.
[[nodiscard]] inline bool is_account_locked(std::string_view locked_until_str,
                                            std::chrono::system_clock::time_point now) noexcept
{
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm{};
#ifdef _WIN32
    gmtime_s(&now_tm, &now_t);
#else
    gmtime_r(&now_t, &now_tm);
#endif
    std::array<char, 32> now_buf{};
    std::strftime(now_buf.data(), now_buf.size(), "%Y-%m-%d %H:%M:%S", &now_tm);
    return locked_until_str > std::string_view(now_buf.data());
}

} // namespace apex::auth_svc
