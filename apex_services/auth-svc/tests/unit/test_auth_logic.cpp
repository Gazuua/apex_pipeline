// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/auth_logic.hpp>

#include <gtest/gtest.h>

#include <chrono>

namespace apex::auth_svc::test
{

namespace
{

/// 특정 UTC 시점을 생성하는 헬퍼.
/// @param year, month, day, hour, min, sec — UTC 기준 날짜/시간.
std::chrono::system_clock::time_point make_utc(int year, int month, int day, int hour = 0, int min = 0, int sec = 0)
{
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0;
#ifdef _WIN32
    auto time_c = _mkgmtime(&tm);
#else
    auto time_c = timegm(&tm);
#endif
    return std::chrono::system_clock::from_time_t(time_c);
}

} // anonymous namespace

// ============================================================
// is_account_locked
// ============================================================

TEST(AuthLogic_IsAccountLocked, FutureDate_Locked)
{
    // locked_until = 2099년, 현재 = 2026년 → 잠금 중
    auto now = make_utc(2026, 3, 22, 12, 0, 0);
    EXPECT_TRUE(is_account_locked("2099-12-31 23:59:59", now));
}

TEST(AuthLogic_IsAccountLocked, PastDate_Unlocked)
{
    // locked_until = 2020년, 현재 = 2026년 → 잠금 해제
    auto now = make_utc(2026, 3, 22, 12, 0, 0);
    EXPECT_FALSE(is_account_locked("2020-01-01 00:00:00", now));
}

TEST(AuthLogic_IsAccountLocked, ExactlySameTime_NotLocked)
{
    // locked_until == now → locked_str > now_buf 는 false (같으면 잠금 아님)
    auto now = make_utc(2026, 3, 22, 12, 0, 0);
    EXPECT_FALSE(is_account_locked("2026-03-22 12:00:00", now));
}

TEST(AuthLogic_IsAccountLocked, OneSecondBefore_NotLocked)
{
    // locked_until이 현재보다 1초 전 → 잠금 해제
    auto now = make_utc(2026, 3, 22, 12, 0, 1);
    EXPECT_FALSE(is_account_locked("2026-03-22 12:00:00", now));
}

TEST(AuthLogic_IsAccountLocked, OneSecondAfter_Locked)
{
    // locked_until이 현재보다 1초 후 → 잠금 중
    auto now = make_utc(2026, 3, 22, 12, 0, 0);
    EXPECT_TRUE(is_account_locked("2026-03-22 12:00:01", now));
}

TEST(AuthLogic_IsAccountLocked, PostgresTimezoneFormat)
{
    // PostgreSQL timestamptz 형식 "+00" 접미사가 있는 경우
    // 문자열 비교에서 "+00" 접미사는 시간 부분 뒤에 오므로 정렬에 영향 없음
    auto now = make_utc(2026, 3, 22, 12, 0, 0);
    // "2099-12-31 23:59:59+00" > "2026-03-22 12:00:00" → true
    EXPECT_TRUE(is_account_locked("2099-12-31 23:59:59+00", now));
}

TEST(AuthLogic_IsAccountLocked, EmptyString_NotLocked)
{
    // DB에서 NULL -> 빈 문자열로 변환된 경우.
    // 빈 문자열은 모든 문자열보다 작으므로 잠금 해제로 판정된다.
    auto now = make_utc(2026, 3, 22, 12, 0, 0);
    EXPECT_FALSE(is_account_locked("", now));
}

} // namespace apex::auth_svc::test
