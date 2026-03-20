#pragma once

/// @file e2e_test_fixture.hpp
/// @brief E2E integration test infrastructure.
///
/// Global lifecycle: Services are assumed to be running in Docker containers.
/// Fixture performs Gateway health check, then runs all tests.
/// Client -> Gateway -> Kafka -> Service -> DB -> Response/Broadcast

#include <gtest/gtest.h>

#include <apex/shared/protocols/tcp/wire_header.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace apex::e2e
{

// Re-export wire_flags for test assertions (e.g. ERROR_RESPONSE)
using namespace apex::shared::protocols::tcp::wire_flags;

using WireHeader = apex::shared::protocols::tcp::WireHeader;

/// Read an integer from environment variable, returning default_val if unset.
/// Centralized to avoid default-value drift across stress test files.
inline int get_env_int(const char* name, int default_val)
{
    const char* val = std::getenv(name);
    return val ? std::atoi(val) : default_val;
}

/// E2E test environment configuration.
/// Ports match docker-compose.e2e.yml definitions.
struct E2EConfig
{
    std::string gateway_host = "127.0.0.1";
    uint16_t gateway_tcp_port = 8443;
    uint16_t gateway_ws_port = 8444;

    // Timeouts
    std::chrono::seconds startup_timeout{30};
    std::chrono::seconds request_timeout{10};

    static E2EConfig from_env();
};

/// Global E2E test environment.
/// Services are assumed to be running in Docker containers.
/// Fixture waits for Gateway to become reachable, then runs all tests.
/// Registered via ::testing::AddGlobalTestEnvironment() in main().
class E2EEnvironment : public ::testing::Environment
{
  public:
    void SetUp() override;
    void TearDown() override;

    static E2EConfig& config()
    {
        return config_;
    }
    static bool is_ready()
    {
        return ready_;
    }

  private:
    static E2EConfig config_;
    static bool ready_;
};

/// E2E test base fixture.
/// Per-test: fresh io_context + helper methods.
/// Infrastructure lifecycle is managed by E2EEnvironment (global).
class E2ETestFixture : public ::testing::Test
{
  public:
    /// TCP client that speaks WireHeader v2 protocol.
    /// Sends and receives framed messages through Gateway.
    class TcpClient
    {
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
        struct Response
        {
            uint32_t msg_id{0};
            uint8_t flags{0};
            std::vector<uint8_t> payload;
        };
        Response recv(std::chrono::seconds timeout = std::chrono::seconds{30});

        /// Close connection
        void close();

        /// Check if connected
        [[nodiscard]] bool is_connected() const noexcept
        {
            return connected_;
        }

      private:
        boost::asio::io_context& io_ctx_;
        E2EConfig config_;
        boost::asio::ip::tcp::socket socket_;
        bool connected_{false};
    };

  protected:
    void SetUp() override;
    void TearDown() override;

    /// JWT login helper (sends LoginRequest through Auth Service pipeline)
    struct AuthResult
    {
        std::string access_token;
        std::string refresh_token;
        uint64_t user_id{0};
        uint32_t expires_in_sec{0};
    };
    AuthResult login(TcpClient& client, const std::string& email, const std::string& password);

    /// Bind JWT to session (sends AuthenticateSession message)
    void authenticate(TcpClient& client, const std::string& token);

    /// Subscribe session to a Redis Pub/Sub channel (system msg_id=4)
    void subscribe_channel(TcpClient& client, const std::string& channel);

    /// Unsubscribe session from a Redis Pub/Sub channel (system msg_id=5)
    void unsubscribe_channel(TcpClient& client, const std::string& channel);

    E2EConfig& config_ = E2EEnvironment::config();
    boost::asio::io_context io_ctx_;
};

} // namespace apex::e2e
