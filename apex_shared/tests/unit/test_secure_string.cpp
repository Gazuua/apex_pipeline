// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#include <apex/shared/secure_string.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <utility>

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
    // verification for testing purposes. ASAN/MSAN may flag this.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_MEMORY__)
    EXPECT_NE(raw_ptr[0], kSecret[0]);
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
