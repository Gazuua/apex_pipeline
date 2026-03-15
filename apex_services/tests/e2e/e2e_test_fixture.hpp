#pragma once

/// @file e2e_test_fixture.hpp
/// @brief E2E integration test infrastructure.
///
/// Manages docker-compose lifecycle, Gateway/Service processes, and
/// provides TCP/WebSocket client helpers for full pipeline testing:
/// Client -> Gateway -> Kafka -> Service -> DB -> Response/Broadcast

#include <gtest/gtest.h>

#include <apex/shared/protocols/tcp/wire_header.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apex::e2e {

using WireHeader = apex::shared::protocols::tcp::WireHeader;

/// E2E test environment configuration.
/// Ports match docker-compose.e2e.yml definitions.
struct E2EConfig {
    std::string gateway_host     = "127.0.0.1";
    uint16_t    gateway_ws_port  = 8443;

    // Redis ports (docker-compose.e2e.yml)
    uint16_t redis_auth_port     = 6380;
    uint16_t redis_ratelimit_port = 6381;
    uint16_t redis_chat_port     = 6382;
    uint16_t redis_pubsub_port   = 6383;

    // Kafka
    std::string kafka_broker     = "localhost:9092";

    // PostgreSQL
    std::string pg_host          = "localhost";
    uint16_t    pg_port          = 5432;
    std::string pg_dbname        = "apex_db";
    std::string pg_user          = "apex_admin";
    std::string pg_password      = "apex_e2e_password";

    // Timeouts
    std::chrono::seconds startup_timeout{30};
    std::chrono::seconds request_timeout{10};
};

/// E2E test base fixture.
///
/// SetUpTestSuite(): docker-compose up + Gateway/Service process launch
/// TearDownTestSuite(): process termination + docker-compose down
class E2ETestFixture : public ::testing::Test {
public:
    static void SetUpTestSuite();
    static void TearDownTestSuite();

protected:
    void SetUp() override;
    void TearDown() override;

    /// TCP client that speaks WireHeader v2 protocol.
    /// Sends and receives framed messages through Gateway.
    class TcpClient {
    public:
        TcpClient(boost::asio::io_context& io_ctx, const E2EConfig& config);
        ~TcpClient();

        TcpClient(const TcpClient&) = delete;
        TcpClient& operator=(const TcpClient&) = delete;

        /// Establish TCP connection to Gateway
        void connect();

        /// Send WireHeader + payload
        void send(uint32_t msg_id, const uint8_t* payload, size_t size);

        /// Receive WireHeader + payload (blocking with timeout)
        struct Response {
            uint32_t msg_id{0};
            uint8_t  flags{0};
            std::vector<uint8_t> payload;
        };
        Response recv(std::chrono::seconds timeout = std::chrono::seconds{10});

        /// Close connection
        void close();

        /// Check if connected
        [[nodiscard]] bool is_connected() const noexcept { return connected_; }

    private:
        boost::asio::io_context& io_ctx_;
        E2EConfig config_;
        boost::asio::ip::tcp::socket socket_;
        bool connected_{false};
    };

    /// JWT login helper (sends LoginRequest through Auth Service pipeline)
    struct AuthResult {
        std::string access_token;
        std::string refresh_token;
        uint64_t user_id{0};
        uint32_t expires_in_sec{0};
    };
    AuthResult login(TcpClient& client, const std::string& email,
                     const std::string& password);

    /// Bind JWT to session (sends AuthenticateSession message)
    void authenticate(TcpClient& client, const std::string& token);

    static E2EConfig config_;
    boost::asio::io_context io_ctx_;
};

} // namespace apex::e2e
