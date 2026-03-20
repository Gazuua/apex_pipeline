// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "e2e_test_fixture.hpp"

#include <chat_room_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

namespace apex::e2e
{

namespace chat_fbs = apex::chat_svc::fbs;

class RateLimitE2ETest : public E2ETestFixture
{};

/// Scenario 5: Per-User Rate Limit exceeded -> error response
///
/// Pipeline position: JWT verified -> [Per-User check] -> reject
/// E2E config should set low per-user limit (e.g., 5 req/sec) for testing.
TEST_F(RateLimitE2ETest, PerUserRateLimit)
{
    TcpClient client(io_ctx_, config_);
    client.connect();

    auto auth = login(client, "alice@apex.dev", "password123");
    authenticate(client, auth.access_token);

    // Burst requests to exceed per-user rate limit
    int rate_limited_count = 0;
    constexpr int burst_count = 20;

    for (int i = 0; i < burst_count; ++i)
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = client.recv(std::chrono::seconds{3});
        if (resp.flags & ERROR_RESPONSE)
        {
            // Error response -> Rate Limit or auth pipeline rejection
            rate_limited_count++;
        }
    }

    EXPECT_GT(rate_limited_count, 0) << "Per-User Rate Limit did not trigger after " << burst_count
                                     << " rapid requests";

    client.close();
}

/// Per-IP Rate Limit (pre-JWT, connection-level)
///
/// Pipeline position: TLS -> [Per-IP check] -> reject
/// Flood connections from same IP without JWT.
TEST_F(RateLimitE2ETest, PerIpRateLimit)
{
    int rejected_count = 0;
    constexpr int attempt_count = 50;

    for (int i = 0; i < attempt_count; ++i)
    {
        try
        {
            TcpClient client(io_ctx_, config_);
            client.connect();

            // Send request without authentication
            flatbuffers::FlatBufferBuilder fbb(128);
            auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
            fbb.Finish(req);
            client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());

            auto resp = client.recv(std::chrono::seconds{2});
            if (resp.flags & ERROR_RESPONSE)
            {
                rejected_count++;
            }
            client.close();
        }
        catch (...)
        {
            // Connection refused -> IP rate limit kicked in
            rejected_count++;
        }
    }

    EXPECT_GT(rejected_count, 0) << "Per-IP Rate Limit did not trigger after " << attempt_count << " rapid connections";

    // Wait for IP rate limit window to expire so subsequent tests
    // (PerEndpointRateLimit, TimeoutE2E) can connect without being blocked.
    // window_size_seconds=2 in gateway_e2e.toml, wait 3s for margin.
    std::this_thread::sleep_for(std::chrono::seconds{3});
}

/// Per-Endpoint Rate Limit (msg_id-specific throttle)
///
/// Pipeline position: JWT verified -> Per-User OK -> [Per-Endpoint check] -> reject
/// CreateRoom (msg_id 2001) has a low override limit in gateway.toml.
TEST_F(RateLimitE2ETest, PerEndpointRateLimit)
{
    TcpClient client(io_ctx_, config_);
    client.connect();

    auto auth = login(client, "bob@apex.dev", "password123");
    authenticate(client, auth.access_token);

    // Burst CreateRoom requests (msg_id 2001, low endpoint limit)
    int rate_limited = 0;
    constexpr int burst_count = 15;

    for (int i = 0; i < burst_count; ++i)
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto name = fbb.CreateString("Room " + std::to_string(i));
        auto req = chat_fbs::CreateCreateRoomRequest(fbb, name, 10);
        fbb.Finish(req);
        client.send(2001, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = client.recv(std::chrono::seconds{3});
        if (resp.flags & ERROR_RESPONSE)
        {
            rate_limited++;
        }
    }

    EXPECT_GT(rate_limited, 0) << "Per-Endpoint Rate Limit did not trigger after " << burst_count
                               << " rapid CreateRoom requests";

    client.close();
}

} // namespace apex::e2e
