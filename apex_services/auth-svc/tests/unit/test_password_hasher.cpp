#include <apex/auth_svc/password_hasher.hpp>

#include <gtest/gtest.h>

using apex::auth_svc::PasswordHasher;

class PasswordHasherTest : public ::testing::Test
{
  protected:
    // work factor 4: optimized for test speed (12 is production)
    PasswordHasher hasher{4};
};

TEST_F(PasswordHasherTest, HashProducesNonEmptyString)
{
    auto hash = hasher.hash("password123");
    EXPECT_FALSE(hash.empty());
    // bcrypt hash starts with "$2a$" or "$2b$"
    EXPECT_TRUE(hash.starts_with("$2a$") || hash.starts_with("$2b$"));
}

TEST_F(PasswordHasherTest, VerifyCorrectPassword)
{
    auto hash = hasher.hash("my_secret");
    EXPECT_TRUE(hasher.verify("my_secret", hash));
}

TEST_F(PasswordHasherTest, VerifyWrongPassword)
{
    auto hash = hasher.hash("correct_password");
    EXPECT_FALSE(hasher.verify("wrong_password", hash));
}

TEST_F(PasswordHasherTest, DifferentHashesForSamePassword)
{
    auto hash1 = hasher.hash("same_password");
    auto hash2 = hasher.hash("same_password");
    // bcrypt uses random salt -> same password yields different hashes
    EXPECT_NE(hash1, hash2);
    // But both should verify
    EXPECT_TRUE(hasher.verify("same_password", hash1));
    EXPECT_TRUE(hasher.verify("same_password", hash2));
}

TEST_F(PasswordHasherTest, EmptyPasswordHandled)
{
    auto hash = hasher.hash("");
    EXPECT_FALSE(hash.empty());
    EXPECT_TRUE(hasher.verify("", hash));
    EXPECT_FALSE(hasher.verify("not_empty", hash));
}
