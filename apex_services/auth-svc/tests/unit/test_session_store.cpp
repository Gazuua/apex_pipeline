#include <apex/auth_svc/crypto_util.hpp>

#include <gtest/gtest.h>

using namespace apex::auth_svc;

// SessionStore depends on RedisAdapter, so unit tests are limited.
// Redis mock or actual Redis integration tests go in integration/ directory.
// Here we test the crypto utility functions used by SessionStore.

TEST(CryptoUtilTest, Sha256HexProducesFixedLength) {
    auto hash = sha256_hex("hello world");
    EXPECT_EQ(hash.size(), 64u);
}

TEST(CryptoUtilTest, Sha256HexDeterministic) {
    auto hash1 = sha256_hex("test input");
    auto hash2 = sha256_hex("test input");
    EXPECT_EQ(hash1, hash2);
}

TEST(CryptoUtilTest, Sha256HexKnownVector) {
    // Known SHA-256 of "hello world"
    auto hash = sha256_hex("hello world");
    EXPECT_EQ(hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST(CryptoUtilTest, Sha256HexDifferentInputs) {
    auto hash1 = sha256_hex("input1");
    auto hash2 = sha256_hex("input2");
    EXPECT_NE(hash1, hash2);
}

TEST(CryptoUtilTest, GenerateSecureTokenLength) {
    auto token = generate_secure_token(32);
    EXPECT_EQ(token.size(), 64u);  // 32 bytes -> 64 hex chars
}

TEST(CryptoUtilTest, GenerateSecureTokenUnique) {
    auto token1 = generate_secure_token();
    auto token2 = generate_secure_token();
    EXPECT_NE(token1, token2);
}

TEST(CryptoUtilTest, GenerateSecureTokenHexChars) {
    auto token = generate_secure_token();
    for (char c : token) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Non-hex character found: " << c;
    }
}
