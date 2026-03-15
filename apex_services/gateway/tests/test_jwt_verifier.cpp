#include <apex/gateway/jwt_verifier.hpp>

#include <jwt-cpp/jwt.h>
#include <gtest/gtest.h>

using namespace apex::gateway;

class JwtVerifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.secret = "test-secret-key-at-least-32-bytes!!";
        config_.algorithm = "HS256";
        config_.clock_skew = std::chrono::seconds{5};
        config_.sensitive_msg_ids = {10, 11};
    }

    std::string make_token(uint64_t uid, int expires_in_seconds = 3600) {
        return jwt::create()
            .set_issuer("apex")
            .set_subject("testuser")
            .set_payload_claim("uid", jwt::claim(static_cast<int64_t>(uid)))
            .set_issued_at(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now()
                + std::chrono::seconds{expires_in_seconds})
            .sign(jwt::algorithm::hs256{config_.secret});
    }

    JwtConfig config_;
};

TEST_F(JwtVerifierTest, ValidToken) {
    JwtVerifier verifier(config_);
    auto token = make_token(12345);
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 12345u);
    EXPECT_EQ(result->username, "testuser");
}

TEST_F(JwtVerifierTest, ExpiredToken) {
    JwtVerifier verifier(config_);
    auto token = make_token(12345, -100);  // Already expired
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, InvalidSignature) {
    JwtVerifier verifier(config_);
    auto token = jwt::create()
        .set_issuer("apex")
        .set_subject("testuser")
        .set_payload_claim("uid", jwt::claim(static_cast<int64_t>(1)))
        .set_expires_at(std::chrono::system_clock::now()
            + std::chrono::hours{1})
        .sign(jwt::algorithm::hs256{"wrong-secret-key-that-is-long!!"});
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, SensitiveMsgId) {
    JwtVerifier verifier(config_);
    EXPECT_TRUE(verifier.is_sensitive(10));
    EXPECT_TRUE(verifier.is_sensitive(11));
    EXPECT_FALSE(verifier.is_sensitive(1000));
}
