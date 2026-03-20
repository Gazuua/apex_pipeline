// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "e2e_test_fixture.hpp"

#include <chat_room_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

namespace apex::e2e
{

namespace chat_fbs = apex::chat_svc::fbs;

class TimeoutE2ETest : public E2ETestFixture
{};

/// Scenario 6: Service timeout -> Gateway returns SERVICE_TIMEOUT
///
/// Strategy: Send a request with an unregistered msg_id (9999) that falls
/// within the chat service routing range [2000, 2999] but has no handler.
/// The Pending Requests Map in Gateway will eventually time out, and
/// the client receives SystemResponse with GatewayError::SERVICE_TIMEOUT
/// (or INVALID_MSG_ID if Gateway detects it before routing).
TEST_F(TimeoutE2ETest, ServiceTimeout)
{
    TcpClient client(io_ctx_, config_);
    client.connect();

    auto auth = login(client, "alice@apex.dev", "password123");
    authenticate(client, auth.access_token);

    // Send request with unhandled msg_id -> will be routed to chat service
    // but no handler exists -> Kafka consumer ignores -> Gateway times out
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        // Use ListRoomsRequest payload (content doesn't matter for timeout test)
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        // msg_id 2999: within chat range, but no registered handler
        client.send(2999, fbb.GetBufferPointer(), fbb.GetSize());
    }

    // Wait for Gateway timeout (gateway.toml request_timeout_ms = 5000 + margin)
    auto resp = client.recv(std::chrono::seconds{15});

    // Gateway should return error response (timeout)
    EXPECT_TRUE(resp.flags & ERROR_RESPONSE) << "Expected error flag for service timeout, msg_id=" << resp.msg_id
                                             << " flags=" << static_cast<int>(resp.flags);

    client.close();
}

/// Supplementary: After a timeout, normal requests still work
/// (Gateway session is not corrupted by the timeout)
///
/// Uses a fresh TCP connection to avoid stream contamination from prior tests'
/// late-arriving Kafka responses on the same Gateway consumer.
TEST_F(TimeoutE2ETest, ServiceRecoveryAfterTimeout)
{
    // Allow Gateway to clean up pending request map from previous ServiceTimeout test.
    // Without this, the new session may inherit timing artifacts on CI.
    std::this_thread::sleep_for(std::chrono::seconds{2});

    // Fresh connection — no stale data possible on a new TCP session, so no flush needed.
    // (Previous flush loop using recv(1s) was incompatible with boost::asio::read() +
    //  SO_RCVTIMEO: EAGAIN retries caused a 30s hang → Broken pipe.)
    TcpClient client(io_ctx_, config_);
    client.connect();

    auto auth = login(client, "alice@apex.dev", "password123");
    authenticate(client, auth.access_token);

    // Normal request -> should succeed
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = client.recv();
        EXPECT_EQ(resp.msg_id, 2008u) << "Normal request should succeed after timeout recovery, got msg_id="
                                      << resp.msg_id;
    }

    client.close();
}

} // namespace apex::e2e
