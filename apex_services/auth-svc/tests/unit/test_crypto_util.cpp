// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/crypto_util.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace apex::auth_svc::test
{

// ============================================================
// sha256_hex
// ============================================================

TEST(CryptoUtil_Sha256Hex, EmptyInput)
{
    auto hash = sha256_hex("");
    EXPECT_EQ(hash.size(), 64u);
    // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(CryptoUtil_Sha256Hex, KnownValue)
{
    // SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    auto hash = sha256_hex("hello");
    EXPECT_EQ(hash.size(), 64u);
    EXPECT_EQ(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST(CryptoUtil_Sha256Hex, Deterministic)
{
    auto hash1 = sha256_hex("test_input_42");
    auto hash2 = sha256_hex("test_input_42");
    EXPECT_EQ(hash1, hash2);
}

TEST(CryptoUtil_Sha256Hex, DifferentInputsDiffer)
{
    auto hash1 = sha256_hex("aaa");
    auto hash2 = sha256_hex("aab");
    EXPECT_NE(hash1, hash2);
}

// ============================================================
// generate_secure_token
// ============================================================

TEST(CryptoUtil_GenerateSecureToken, DefaultSize)
{
    auto result = generate_secure_token();
    ASSERT_TRUE(result.has_value());
    // 32 bytes → 64 hex chars
    EXPECT_EQ(result->size(), 64u);
}

TEST(CryptoUtil_GenerateSecureToken, CustomSize)
{
    auto result = generate_secure_token(16);
    ASSERT_TRUE(result.has_value());
    // 16 bytes → 32 hex chars
    EXPECT_EQ(result->size(), 32u);
}

TEST(CryptoUtil_GenerateSecureToken, UniqueTokens)
{
    std::set<std::string> tokens;
    for (int i = 0; i < 100; ++i)
    {
        auto result = generate_secure_token();
        ASSERT_TRUE(result.has_value());
        tokens.insert(*result);
    }
    // 100개 토큰이 모두 다른지 확인 (확률적으로 충돌 불가)
    EXPECT_EQ(tokens.size(), 100u);
}

TEST(CryptoUtil_GenerateSecureToken, HexFormat)
{
    auto result = generate_secure_token();
    ASSERT_TRUE(result.has_value());
    for (char c : *result)
    {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "Non-hex character: " << c;
    }
}

} // namespace apex::auth_svc::test
