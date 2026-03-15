#include <apex/auth_svc/jwt_manager.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using apex::auth_svc::JwtManager;

class JwtManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Test RSA keys: skip if not available
        if (!std::filesystem::exists("test_keys/private.pem")) {
            GTEST_SKIP() << "Test RSA keys not found in test_keys/";
        }

        manager_ = std::make_unique<JwtManager>(
            "test_keys/private.pem",
            "test_keys/public.pem",
            "test-issuer",
            std::chrono::seconds{300}  // 5 min
        );
    }

    std::unique_ptr<JwtManager> manager_;
};

TEST_F(JwtManagerTest, CreateAndVerifyToken) {
    auto token = manager_->create_access_token(42, "test@apex.dev");
    EXPECT_FALSE(token.empty());

    auto claims = manager_->verify_access_token(token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->user_id, 42);
    EXPECT_EQ(claims->email, "test@apex.dev");
}

TEST_F(JwtManagerTest, InvalidTokenFails) {
    auto result = manager_->verify_access_token("invalid.jwt.token");
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtManagerTest, TamperedTokenFails) {
    auto token = manager_->create_access_token(1, "user@apex.dev");
    // Tamper with payload
    auto tampered = token;
    if (tampered.size() > 10) {
        tampered[tampered.size() / 2] ^= 0xFF;
    }
    auto result = manager_->verify_access_token(tampered);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtManagerTest, RemainingTtlPositive) {
    auto token = manager_->create_access_token(1, "user@apex.dev");
    auto ttl = manager_->remaining_ttl(token);
    EXPECT_GT(ttl.count(), 0);
    EXPECT_LE(ttl.count(), 300);
}
