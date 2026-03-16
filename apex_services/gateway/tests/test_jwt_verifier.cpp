#include <apex/gateway/jwt_verifier.hpp>

#include <jwt-cpp/jwt.h>
#include <picojson/picojson.h>
#include <gtest/gtest.h>

#include <fstream>

using namespace apex::gateway;

namespace {

// Minimal 2048-bit RSA key pair for testing only.
// Generated via: openssl genrsa 2048 / openssl rsa -pubout
constexpr const char* kTestPrivateKey = R"(-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEA0Z3VS5JJcds3xfn/yGaXLMU1VscGwCJqLqMAcGNHMVRMBEzN
cSW4pAjTDV2GaFOBXwPQ9qnPBKbACMJVlNLGSYlHUKna3WROnVuPOPJjCEVR1G7j
PNNu5jCFGo9bN5lWLtYGnkJMJKc1H85YYMiPYso6cR9YRe3oEjy0VxpEQBR8jM7p
MaJBqGHKDO7g7rGIsh2Mi0Qa4H55CiY2TMdGBy/3dOJRH0WEwXFh9cGz3vMHVNPa
ExVtJbOmFdoF0AdPIFejPO3MH9ou3LjGSIDZQxS7KZhhR/3lIDHwVX2pVJjA6nhs
NzdqVY+7EHSkj6WWRREP1Cq7CVRwBfSJ3SQhcwIDAQABAoIBAEwB3X2K6k0WhNbM
NdJN1fGo1bOrVCiKiPt2bWKQCbpO2WdZJOBpWw9J6JOD2g3NJ3bVNXSqVKF3LXa8
SpPQx3F7BdFGYBsiV3sSDFkiToLj5vYEUBGMbLVMMRVoMXfKQ+KsBnWElqFSfqCK
m+rONj0ZjgT+DTMXFT76QDh7gZT0LSaqSW8DVzAU7e7hCAc4oQatmmYGg2Aih+U+
L95PaW8FXHKP1kBWJ0C8TOcF3H3zMNyXsYKXBnTGHSjYMQJHpGhW8RQN1H3BO8u
0VREr7WnpRABfX4pVTPWjQz35i42j+WXoV1q/hFRjxmG/d/UJHZkl3E8CgPCK22T
I+3gmpECgYEA7mGC9K9AjKnEoNfCQKHIJwWm8RW9G3MZ8dOBeaW+g/RLNC+XPIoi
n0C9IxDF/1zPRYVm+JKjGN7S9FJqfkfSNMU/SDHDRJkjH8EC3HFmhDTVHUGXLxXP
BYFqEXSgHHGjMMdFPKAn8xfD0H6ssfTX3WjRBqAPaOka2vSdq3EfTtkCgYEA4R4A
gOiOZGlGa+7PNJXqEOJRW+hQsip7C3a7G8OxgFlXzZvJPO/OGXcVB5SqXC5RDZBI
xN/GhbxHGYTYlOJLfwRCY1VNjnXI7VEiNUY4e08M5NHOPfM5ZPEgSP6VuKPLj/6F
yN+LbSADN6b2ZfFJM+GLkme/0py8p1FCn6pGb1sCgYBSxrIOrZq1TPaI0VPXMeCR
dPPKOB2GQO0FS03wD/ZsBDsLZa4w6Y5JgYXP3TrEC/sBPkjq4aPaGl3W+Fp1JqNw
v7fO0LVjFMiYq/qN4l+mWUOEvjK+HX8k7qCG8q4OGbw2j9z0GneJ1MGx4UP7qJP3
8Fvr7M3XOa6LOglLSr8seQKBgHzxkXrHLXb6D2kDa0gQ5z6LEdL1stNL1nWFcHJh
N3LGC/x6wP5Gd7qFl6XO6DsXwpJlIWsnRFbZB9pgLtFWJp0x5vcBo/VBeFWJdxaR
L/rZ8zq0v8m+07qwkNGhACXl6pLGJFvQE0M5JLxwi9I6p48+jwG8dYLKw3U+QJC2
f2Z7AoGBAMQrZ5bvdfUkxaAsmFtJBqsR1MPb6AcDUTY0T3KKb2H2M0x9RWuNR+UH
ZIpG7PnH+4OFH7kw8gA2ZlrJRcCdjEZpHnbyTTPbTVNEBrjJGPTMemIW6ZK0ofE7
G4GQBeTfUVJD1UqWK3pHPjEDGAT6DOl+tZmqjFPaVVxHfg5nUI1T
-----END RSA PRIVATE KEY-----
)";

constexpr const char* kTestPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Z3VS5JJcds3xfn/yGaX
LMU1VscGwCJqLqMAcGNHMVRMBEzNcSW4pAjTDV2GaFOBXwPQ9qnPBKbACMJVlNLG
SYlHUKna3WROnVuPOPJjCEVR1G7jPNNu5jCFGo9bN5lWLtYGnkJMJKc1H85YYMiP
Yso6cR9YRe3oEjy0VxpEQBR8jM7pMaJBqGHKDO7g7rGIsh2Mi0Qa4H55CiY2TMdG
By/3dOJRH0WEwXFh9cGz3vMHVNPaExVtJbOmFdoF0AdPIFejPO3MH9ou3LjGSIDZ
QxS7KZhhR/3lIDHwVX2pVJjA6nhsNzdqVY+7EHSkj6WWRREP1Cq7CVRwBfSJ3SQh
cwIDAQAB
-----END PUBLIC KEY-----
)";

} // anonymous namespace

class JwtVerifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Write test public key to a temp file for JwtVerifier to load
        test_key_path_ = ::testing::TempDir() + "test_rs256_pub.pem";
        std::ofstream ofs(test_key_path_);
        ofs << kTestPublicKey;
        ofs.close();

        config_.public_key_file = test_key_path_;
        config_.algorithm = "RS256";
        config_.issuer = "apex-auth";
        config_.clock_skew = std::chrono::seconds{5};
        config_.sensitive_msg_ids = {10, 11};
    }

    void TearDown() override {
        std::remove(test_key_path_.c_str());
    }

    std::string make_token(uint64_t uid,
                           std::string_view email = "test@example.com",
                           int expires_in_seconds = 3600) {
        auto now = std::chrono::system_clock::now();
        return jwt::create()
            .set_issuer("apex-auth")
            .set_type("JWT")
            .set_subject(std::string(email))
            .set_payload_claim("uid",
                jwt::claim(picojson::value(static_cast<double>(uid))))
            .set_payload_claim("jti",
                jwt::claim(std::string("test-jti-001")))
            .set_issued_at(now)
            .set_expires_at(now + std::chrono::seconds{expires_in_seconds})
            .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    }

    JwtConfig config_;
    std::string test_key_path_;
};

TEST_F(JwtVerifierTest, ValidToken) {
    JwtVerifier verifier(config_);
    auto token = make_token(12345);
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 12345u);
    EXPECT_EQ(result->email, "test@example.com");
    EXPECT_EQ(result->jti, "test-jti-001");
}

TEST_F(JwtVerifierTest, ExpiredToken) {
    JwtVerifier verifier(config_);
    auto token = make_token(12345, "test@example.com", -100);  // Already expired
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, InvalidIssuer) {
    // Token signed with correct key but wrong issuer -- issuer validation should reject
    JwtVerifier verifier(config_);
    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
        .set_issuer("wrong-issuer")
        .set_type("JWT")
        .set_subject("test@example.com")
        .set_payload_claim("uid",
            jwt::claim(picojson::value(static_cast<double>(1))))
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::hours{1})
        .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, InvalidSignature) {
    // Tamper with a valid token to break the signature
    JwtVerifier verifier(config_);
    auto token = make_token(12345);
    // Flip a character in the signature portion (after last '.')
    auto last_dot = token.rfind('.');
    ASSERT_NE(last_dot, std::string::npos);
    ASSERT_GT(token.size(), last_dot + 1);
    token[last_dot + 1] ^= 0x01;  // flip one bit
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, SensitiveMsgId) {
    JwtVerifier verifier(config_);
    EXPECT_TRUE(verifier.is_sensitive(10));
    EXPECT_TRUE(verifier.is_sensitive(11));
    EXPECT_FALSE(verifier.is_sensitive(1000));
}

TEST_F(JwtVerifierTest, ClaimParsing_UidAsNumber) {
    // Verify uid is parsed correctly as number (Auth stores as double via picojson)
    JwtVerifier verifier(config_);
    auto token = make_token(9999999);
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 9999999u);
}

TEST_F(JwtVerifierTest, MissingJti) {
    // Token without jti should still verify -- jti is optional
    JwtVerifier verifier(config_);
    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
        .set_issuer("apex-auth")
        .set_type("JWT")
        .set_subject("nojti@example.com")
        .set_payload_claim("uid",
            jwt::claim(picojson::value(static_cast<double>(42))))
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::hours{1})
        .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 42u);
    EXPECT_EQ(result->email, "nojti@example.com");
    EXPECT_TRUE(result->jti.empty());
}
