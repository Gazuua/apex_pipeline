// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/jwt_blacklist.hpp>

#include <gtest/gtest.h>

#include <string>

using apex::gateway::detail::is_valid_jti;

// --- Valid inputs ---

TEST(JwtBlacklist, ValidHexLowercase)
{
    EXPECT_TRUE(is_valid_jti("a1b2c3d4e5f6"));
}

TEST(JwtBlacklist, ValidHexUppercase)
{
    EXPECT_TRUE(is_valid_jti("A1B2C3D4E5F6"));
}

TEST(JwtBlacklist, ValidHexMixed)
{
    EXPECT_TRUE(is_valid_jti("aAbBcCdDeEfF0123456789"));
}

TEST(JwtBlacklist, ValidWithHyphens)
{
    EXPECT_TRUE(is_valid_jti("550e8400-e29b-41d4-a716-446655440000"));
}

TEST(JwtBlacklist, ValidSingleChar)
{
    EXPECT_TRUE(is_valid_jti("a"));
}

TEST(JwtBlacklist, ValidExactly128Chars)
{
    // 128 hex chars — boundary
    std::string jti(128, 'a');
    EXPECT_TRUE(is_valid_jti(jti));
}

// --- Invalid inputs ---

TEST(JwtBlacklist, RejectEmpty)
{
    EXPECT_FALSE(is_valid_jti(""));
}

TEST(JwtBlacklist, RejectExceeds128Chars)
{
    std::string jti(129, 'a');
    EXPECT_FALSE(is_valid_jti(jti));
}

TEST(JwtBlacklist, RejectSpace)
{
    EXPECT_FALSE(is_valid_jti("a1b2 c3d4"));
}

TEST(JwtBlacklist, RejectUnderscore)
{
    EXPECT_FALSE(is_valid_jti("a1b2_c3d4"));
}

TEST(JwtBlacklist, RejectAtSign)
{
    EXPECT_FALSE(is_valid_jti("a1b2@c3d4"));
}

TEST(JwtBlacklist, RejectOutOfHexRange)
{
    // 'g' is not a valid hex digit
    EXPECT_FALSE(is_valid_jti("g1b2c3d4"));
}

TEST(JwtBlacklist, RejectNewline)
{
    EXPECT_FALSE(is_valid_jti("a1b2\nc3d4"));
}

TEST(JwtBlacklist, RejectRedisInjection)
{
    // Attempt Redis command injection via CRLF
    EXPECT_FALSE(is_valid_jti("abc\r\nDEL jwt:blacklist:*"));
}
