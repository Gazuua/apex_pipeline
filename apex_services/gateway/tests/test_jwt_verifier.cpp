// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/jwt_verifier.hpp>

#include <gtest/gtest.h>
#include <jwt-cpp/jwt.h>

#include <fstream>

using namespace apex::gateway;

namespace
{

// Minimal 2048-bit RSA key pair for testing only.
// Generated via: openssl genrsa 2048 / openssl rsa -pubout
constexpr const char* kTestPrivateKey = R"(-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEAl6f3nQbd8zYwBU17b87v6uXMcQ4z45jYk6k6DHm+ZYYXTGZx
RDs7brGgAzwL7y5EWEuXnu4ZZweu72e8vJyb562B1tbNjvGXjMHqOq6Gwe79v6d+
eVn8VMm8YHHRt6pk9H8JG5YZOlNy4yBvONrViGDjwlIOM07s45EhT09OHL7lhIPV
zrzWmDShS1VpJ+/aCpfwr+fEv4TcR/Cy85LdI1ShaGnn9NNeUWy2E233OJ5JSOW4
ZRmQjcYX9qEWOhVZIfBPPcaNmFbAUg1/uNh7Tvggab4jP8CatHP54rBx1P/LBTFV
id7saTS2URyRizsF0VUekO7CULdV6NO5oR5J7QIDAQABAoIBAAPeMVcMb0m+NpBU
DxNldEIJ9WJmydUCt3dgqA3cZwpA1sRIkpf6aMQkfJ/IfsdtZynXV7kienxhukj5
3Nr0pZdNmhOoK9r693IMGhTAEcjNdyRVuciPjnt9H7RYAtDL5EJGIS3WcLNHoJUB
ZUTOdAbD7dsta5ZMa4dMUsyqiaP7AmMEpMeW/4dg8SSoGv0ScRy9F6qp3JqJpJpO
BRxodOVIl/DYF5wDTuVbv4R+0eIsOTLrASJ//9u/bbLHyAon/e6khBt1pYzldea1
wuLS4r4vdyUTpe9tp6YnaTMEgq+4Eok1HD6MnrRs+Dj0jzIibFpMUmB31kZhOptf
37aLFzMCgYEAyQxlZKm5Gf9pSJvJhBrbozPpZuGlNLiUdMD8KDT4tVc8PsUBDeTg
QDE99TAdogM0T3vPDyiGFMQAkkj7iMyU1RA7V6aznAykjK4mBOp/wSZeDDb0DIqc
arVGiOzYxbv9fUE34RdVc3cHK/2FI9KMowVsNMPPj9AJP78i2ut14p8CgYEAwRuI
EusAp8lZZScfg8UbOy0HxB5uyKIziq83aEul+oC+hmjmI/GjLy1U/B84/sDYku9w
3J6m4SO06bvRE4Z2KhHKmbWW9l0O9/W9uuOEXctzgnbSgqu1XgylYn9/WL7p6bY+
nLP0kOhtADEC/1fKuMJ2yQ7emthcdN+4NUFds/MCgYEAsoToT28JZpVNlmSGlmAG
4S7KNElumZbAc7+c59LJeLRCUXY6zmyJ11YiLuIPnfl8bIuCO3J/RFcaLsrRVxJZ
oastFlJ2r7zmK+jC56CV2htIbU4qfCxkYbgfLpwoi8O2fY74oE1I2iM54gzWOQ3P
RT4ea+fnGUrfu5Przjo/zf8CgYBuc33gPDQyxBLyrW70IpBRx47SmQmKjmPmphON
/v7fijXvkR5ZXsOUn1wlnQIvjEQTvwqR1djjm1XF/tw2S8lYhLaaNmgzX8TJBPDR
bYLvVwgpjicYAHiLY7ZQ4VYIf6IxQEENxkxTee2ml2H8hM154hipJW0jqi8v1ip5
o3qiCwKBgA3vtHUSC/jKRVwIfE7B2XHCUW13kF7Px98ehJh7uz9XzkaiWgCX0GBP
Bk/XE8JgZ6YDQaL9iBef3P4oyC+xsxewgkALqR+GYI5rrLaH+QHsSc2BrgeQcBwG
QthCqBNBG1fU1OAop3+GIBmeNf0w+DOj+ILy2MF/7fAf8RTETC8S
-----END RSA PRIVATE KEY-----
)";

constexpr const char* kTestPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAl6f3nQbd8zYwBU17b87v
6uXMcQ4z45jYk6k6DHm+ZYYXTGZxRDs7brGgAzwL7y5EWEuXnu4ZZweu72e8vJyb
562B1tbNjvGXjMHqOq6Gwe79v6d+eVn8VMm8YHHRt6pk9H8JG5YZOlNy4yBvONrV
iGDjwlIOM07s45EhT09OHL7lhIPVzrzWmDShS1VpJ+/aCpfwr+fEv4TcR/Cy85Ld
I1ShaGnn9NNeUWy2E233OJ5JSOW4ZRmQjcYX9qEWOhVZIfBPPcaNmFbAUg1/uNh7
Tvggab4jP8CatHP54rBx1P/LBTFVid7saTS2URyRizsF0VUekO7CULdV6NO5oR5J
7QIDAQAB
-----END PUBLIC KEY-----
)";

} // anonymous namespace

class JwtVerifierTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Write test public key to a temp file for JwtVerifier to load
        test_key_path_ = ::testing::TempDir() + "test_rs256_pub.pem";
        std::ofstream ofs(test_key_path_);
        ofs << kTestPublicKey;
        ofs.close();

        config_.public_key_file = test_key_path_;
        config_.algorithm = "RS256";
        config_.issuer = "apex-auth";
        config_.clock_skew = std::chrono::seconds{5};
        config_.sensitive_msg_ids = {1004, 1005};
    }

    void TearDown() override
    {
        std::remove(test_key_path_.c_str());
    }

    std::string make_token(uint64_t uid, std::string_view email = "test@example.com", int expires_in_seconds = 3600)
    {
        auto now = std::chrono::system_clock::now();
        return jwt::create()
            .set_issuer("apex-auth")
            .set_type("JWT")
            .set_subject(std::string(email))
            .set_payload_claim("uid", jwt::claim(std::to_string(uid)))
            .set_payload_claim("jti", jwt::claim(std::string("test-jti-001")))
            .set_issued_at(now)
            .set_expires_at(now + std::chrono::seconds{expires_in_seconds})
            .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    }

    JwtConfig config_;
    std::string test_key_path_;
};

TEST_F(JwtVerifierTest, ValidToken)
{
    JwtVerifier verifier(config_);
    auto token = make_token(12345);
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 12345u);
    EXPECT_EQ(result->email, "test@example.com");
    EXPECT_EQ(result->jti, "test-jti-001");
}

TEST_F(JwtVerifierTest, ExpiredToken)
{
    JwtVerifier verifier(config_);
    auto token = make_token(12345, "test@example.com", -100); // Already expired
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, InvalidIssuer)
{
    // Token signed with correct key but wrong issuer -- issuer validation should reject
    JwtVerifier verifier(config_);
    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
                     .set_issuer("wrong-issuer")
                     .set_type("JWT")
                     .set_subject("test@example.com")
                     .set_payload_claim("uid", jwt::claim(std::to_string(1)))
                     .set_issued_at(now)
                     .set_expires_at(now + std::chrono::hours{1})
                     .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, InvalidSignature)
{
    // Tamper with a valid token to break the signature
    JwtVerifier verifier(config_);
    auto token = make_token(12345);
    // Flip a character in the signature portion (after last '.')
    auto last_dot = token.rfind('.');
    ASSERT_NE(last_dot, std::string::npos);
    ASSERT_GT(token.size(), last_dot + 1);
    token[last_dot + 1] ^= 0x01; // flip one bit
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, SensitiveMsgId)
{
    JwtVerifier verifier(config_);
    EXPECT_TRUE(verifier.is_sensitive(1004));
    EXPECT_TRUE(verifier.is_sensitive(1005));
    EXPECT_FALSE(verifier.is_sensitive(1000));
}

TEST_F(JwtVerifierTest, ClaimParsing_UidAsString)
{
    // Verify uid is parsed correctly as string (avoids double precision loss)
    JwtVerifier verifier(config_);
    auto token = make_token(9999999);
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 9999999u);
}

TEST_F(JwtVerifierTest, MissingJti)
{
    // Token without jti should still verify -- jti is optional
    JwtVerifier verifier(config_);
    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
                     .set_issuer("apex-auth")
                     .set_type("JWT")
                     .set_subject("nojti@example.com")
                     .set_payload_claim("uid", jwt::claim(std::to_string(42)))
                     .set_issued_at(now)
                     .set_expires_at(now + std::chrono::hours{1})
                     .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    auto result = verifier.verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->user_id, 42u);
    EXPECT_EQ(result->email, "nojti@example.com");
    EXPECT_TRUE(result->jti.empty());
}

TEST_F(JwtVerifierTest, MissingKeyFile_VerifyFails)
{
    // read_file() fails -> empty public key -> verify should fail
    JwtConfig bad_config = config_;
    bad_config.public_key_file = "/nonexistent/path/pub.pem";
    JwtVerifier verifier(bad_config);

    auto token = make_token(12345);
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, MalformedUidClaim)
{
    // Token with non-numeric uid -> from_chars returns errc, not an exception
    JwtVerifier verifier(config_);
    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
                     .set_issuer("apex-auth")
                     .set_type("JWT")
                     .set_subject("bad-uid@example.com")
                     .set_payload_claim("uid", jwt::claim(std::string("not-a-number")))
                     .set_payload_claim("jti", jwt::claim(std::string("jti-bad-uid")))
                     .set_issued_at(now)
                     .set_expires_at(now + std::chrono::hours{1})
                     .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, MissingUidClaim)
{
    // Token without uid claim -> claim_not_present_exception path
    JwtVerifier verifier(config_);
    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
                     .set_issuer("apex-auth")
                     .set_type("JWT")
                     .set_subject("nouid@example.com")
                     .set_payload_claim("jti", jwt::claim(std::string("jti-no-uid")))
                     .set_issued_at(now)
                     .set_expires_at(now + std::chrono::hours{1})
                     .sign(jwt::algorithm::rs256(kTestPublicKey, kTestPrivateKey));
    auto result = verifier.verify(token);
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, GarbageToken)
{
    // Completely invalid token string -> std::exception catch path
    JwtVerifier verifier(config_);
    auto result = verifier.verify("not-a-jwt-token-at-all");
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtVerifierTest, EmptyToken)
{
    // Empty string token
    JwtVerifier verifier(config_);
    auto result = verifier.verify("");
    EXPECT_FALSE(result.has_value());
}
