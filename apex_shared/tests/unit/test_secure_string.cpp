// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#include <apex/shared/secure_string.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <utility>

// Valgrind 런타임 감지 — ZeroizesOnDestruction 테스트에서 해제된 메모리 접근 가드
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define APEX_RUNNING_ON_VALGRIND RUNNING_ON_VALGRIND
#else
#define APEX_RUNNING_ON_VALGRIND 0
#endif

using apex::shared::SecureString;

TEST(SecureStringTest, DefaultConstructedIsEmpty)
{
    SecureString s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_STREQ(s.c_str(), "");
}

TEST(SecureStringTest, ConstructFromStringView)
{
    SecureString s(std::string_view{"hunter2"});
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.size(), 7u);
    EXPECT_STREQ(s.c_str(), "hunter2");
    EXPECT_EQ(s.view(), "hunter2");
}

TEST(SecureStringTest, ConstructFromRvalueString)
{
    std::string orig = "secret_password";
    SecureString s(std::move(orig));
    EXPECT_STREQ(s.c_str(), "secret_password");
    EXPECT_EQ(s.size(), 15u);
}

TEST(SecureStringTest, MoveConstruction)
{
    SecureString a(std::string_view{"moveme"});
    SecureString b(std::move(a));

    EXPECT_STREQ(b.c_str(), "moveme");
    EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(SecureStringTest, MoveAssignment)
{
    SecureString a(std::string_view{"first"});
    SecureString b(std::string_view{"second"});
    b = std::move(a);

    EXPECT_STREQ(b.c_str(), "first");
    EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(SecureStringTest, CopyCreatesIndependentZeroizedCopy)
{
    SecureString a(std::string_view{"secret_value"});
    SecureString b(a); // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_STREQ(b.c_str(), "secret_value");
    EXPECT_STREQ(a.c_str(), "secret_value"); // original unchanged
}

TEST(SecureStringTest, ZeroizesOnDestruction)
{
    // Allocate a string long enough to bypass SSO (> 15 chars typically)
    constexpr std::string_view kSecret = "a_long_secret_that_bypasses_sso_buffer_1234567890";
    const char* raw_ptr = nullptr;

    {
        SecureString s(kSecret);
        raw_ptr = s.c_str();
        // Verify the secret is there before destruction
        ASSERT_EQ(std::memcmp(raw_ptr, kSecret.data(), kSecret.size()), 0);
    }
    // After destruction, the memory should be zeroed.
    // NOTE: Accessing freed memory is technically UB, but this is a best-effort
    // verification for testing purposes. Skipped under sanitizers and Valgrind.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_MEMORY__) && !defined(__SANITIZE_THREAD__)
    if (!APEX_RUNNING_ON_VALGRIND)
    {
        EXPECT_NE(raw_ptr[0], kSecret[0]);
    }
#endif
}

TEST(SecureStringTest, SelfMoveAssignment)
{
    SecureString s(std::string_view{"self"});
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    s = std::move(s); // NOLINT(bugprone-use-after-move)
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    // Should not crash — either preserved or empty is acceptable
    (void)s.empty();
}

// ============================================================
// Construct from const char*
// ============================================================

TEST(SecureStringTest, ConstructFromConstCharPtr)
{
    SecureString s("normal");
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.size(), 6u);
    EXPECT_STREQ(s.c_str(), "normal");
    EXPECT_EQ(s.view(), "normal");
}

TEST(SecureStringTest, ConstructFromEmptyConstCharPtr)
{
    SecureString s("");
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_STREQ(s.c_str(), "");
}

TEST(SecureStringTest, ConstructFromNullptr)
{
    const char* p = nullptr;
    SecureString s(p);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
}

// ============================================================
// Copy assignment
// ============================================================

TEST(SecureStringTest, CopyAssignmentAfterClear)
{
    SecureString a(std::string_view{"alpha"});
    SecureString b;
    b = a;
    EXPECT_STREQ(b.c_str(), "alpha");
    EXPECT_STREQ(a.c_str(), "alpha"); // original unchanged
}

TEST(SecureStringTest, CopyAssignmentOverwrite)
{
    SecureString a(std::string_view{"first_value"});
    SecureString b(std::string_view{"second_value"});
    b = a;
    EXPECT_STREQ(b.c_str(), "first_value");
    EXPECT_STREQ(a.c_str(), "first_value");
}

// ============================================================
// Equality with SecureString
// ============================================================

TEST(SecureStringTest, EqualityWithSecureStringSameValue)
{
    SecureString a(std::string_view{"same"});
    SecureString b(std::string_view{"same"});
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(SecureStringTest, EqualityWithSecureStringDifferentValue)
{
    SecureString a(std::string_view{"aaa"});
    SecureString b(std::string_view{"bbb"});
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

// ============================================================
// Equality with string_view
// ============================================================

TEST(SecureStringTest, EqualityWithStringViewSameValue)
{
    SecureString s(std::string_view{"hello"});
    EXPECT_TRUE(s == std::string_view{"hello"});
    EXPECT_FALSE(s != std::string_view{"hello"});
}

TEST(SecureStringTest, EqualityWithStringViewDifferentValue)
{
    SecureString s(std::string_view{"hello"});
    EXPECT_FALSE(s == std::string_view{"world"});
    EXPECT_TRUE(s != std::string_view{"world"});
}

// ============================================================
// Equality with const char*
// ============================================================

TEST(SecureStringTest, EqualityWithConstCharPtrSameValue)
{
    SecureString s(std::string_view{"test"});
    EXPECT_TRUE(s == "test");
    EXPECT_FALSE(s != "test");
}

TEST(SecureStringTest, EqualityWithConstCharPtrDifferentValue)
{
    SecureString s(std::string_view{"test"});
    EXPECT_FALSE(s == "other");
    EXPECT_TRUE(s != "other");
}

TEST(SecureStringTest, EqualityWithConstCharPtrNullptr)
{
    SecureString s(std::string_view{"notnull"});
    const char* p = nullptr;
    EXPECT_FALSE(s == p);
    EXPECT_TRUE(s != p);
}

// ============================================================
// Inequality operators (all overloads)
// ============================================================

TEST(SecureStringTest, InequalityOperatorsSecureString)
{
    SecureString a(std::string_view{"x"});
    SecureString b(std::string_view{"y"});
    SecureString c(std::string_view{"x"});
    EXPECT_TRUE(a != b);
    EXPECT_FALSE(a != c);
}

TEST(SecureStringTest, InequalityOperatorsStringView)
{
    SecureString s(std::string_view{"abc"});
    EXPECT_TRUE(s != std::string_view{"xyz"});
    EXPECT_FALSE(s != std::string_view{"abc"});
}

TEST(SecureStringTest, InequalityOperatorsConstCharPtr)
{
    SecureString s(std::string_view{"abc"});
    EXPECT_TRUE(s != "xyz");
    EXPECT_FALSE(s != "abc");
}

// ============================================================
// constant_time_equal (timing-safe comparison)
// ============================================================

TEST(SecureStringTest, ConstantTimeEqualSameValue)
{
    SecureString a(std::string_view{"secret_token_12345"});
    SecureString b(std::string_view{"secret_token_12345"});
    EXPECT_TRUE(a.constant_time_equal(b));
}

TEST(SecureStringTest, ConstantTimeEqualDifferentValue)
{
    SecureString a(std::string_view{"secret_token_12345"});
    SecureString b(std::string_view{"secret_token_12346"});
    EXPECT_FALSE(a.constant_time_equal(b));
}

TEST(SecureStringTest, ConstantTimeEqualDifferentLength)
{
    SecureString a(std::string_view{"short"});
    SecureString b(std::string_view{"longer_string"});
    EXPECT_FALSE(a.constant_time_equal(b));
}

TEST(SecureStringTest, ConstantTimeEqualWithStringView)
{
    SecureString s(std::string_view{"matching_value"});
    EXPECT_TRUE(s.constant_time_equal(std::string_view{"matching_value"}));
    EXPECT_FALSE(s.constant_time_equal(std::string_view{"different_value"}));
}

TEST(SecureStringTest, ConstantTimeEqualEmptyStrings)
{
    SecureString a;
    SecureString b;
    EXPECT_TRUE(a.constant_time_equal(b));
    EXPECT_TRUE(a.constant_time_equal(std::string_view{""}));
}

TEST(SecureStringTest, ConstantTimeEqualEmptyVsNonEmpty)
{
    SecureString a;
    SecureString b(std::string_view{"notempty"});
    EXPECT_FALSE(a.constant_time_equal(b));
    EXPECT_FALSE(b.constant_time_equal(a));
}
