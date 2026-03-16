#include "e2e_test_fixture.hpp"

#include <flatbuffers/flatbuffers.h>
#include <chat_room_generated.h>

#include <gtest/gtest.h>

#include <thread>

namespace apex::e2e {

namespace chat_fbs = apex::chat_svc::fbs;

class AuthE2ETest : public E2ETestFixture {};

/// Scenario 1: Login -> JWT issue -> Authenticated request -> Response
///
/// Full pipeline: Client -> Gateway -> Kafka auth.requests -> Auth Service
///                -> Kafka auth.responses -> Gateway -> Client (JWT)
///                -> Authenticated request -> Gateway validates JWT
///                -> Kafka chat.requests -> Chat Service -> Response
TEST_F(AuthE2ETest, LoginAndAuthenticatedRequest) {
    TcpClient client(io_ctx_, config_);
    client.connect();

    // 1. Login request (msg_id 1000 -> Auth Service)
    auto auth = login(client, "alice@apex.dev", "password123");
    ASSERT_FALSE(auth.access_token.empty()) << "JWT not issued";
    ASSERT_GT(auth.user_id, 0u);

    // 2. Bind JWT to session
    authenticate(client, auth.access_token);

    // 3. Authenticated request -- list chat rooms (msg_id 2007 -> Chat Service)
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());
    }

    // 4. Receive response (msg_id 2008 = ListRoomsResponse)
    auto resp = client.recv();
    EXPECT_EQ(resp.msg_id, 2008u);

    auto* list_resp = flatbuffers::GetRoot<chat_fbs::ListRoomsResponse>(
        resp.payload.data());
    EXPECT_EQ(list_resp->error(), chat_fbs::ChatRoomError_NONE);

    client.close();
}

/// Unauthenticated request -> Gateway rejects with system error
TEST_F(AuthE2ETest, UnauthenticatedRequestRejected) {
    TcpClient client(io_ctx_, config_);
    client.connect();

    // Send request without JWT binding
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());
    }

    // Gateway should return system error (msg_id < 1000)
    auto resp = client.recv();
    EXPECT_LT(resp.msg_id, 1000u)
        << "Expected system error msg_id, got " << resp.msg_id;
    // SystemResponse should contain GatewayError::JWT_INVALID
    // Exact parsing depends on SystemResponse FlatBuffers schema

    client.close();
}

/// Scenario 4: JWT expired -> Refresh Token renewal
///
/// Access Token expires -> client sends RefreshTokenRequest (msg_id 1004)
/// -> Auth Service validates Refresh Token -> new Access Token issued
TEST_F(AuthE2ETest, RefreshTokenRenewal) {
    TcpClient client(io_ctx_, config_);
    client.connect();

    // 1. Login -> get Access Token + Refresh Token
    auto auth = login(client, "alice@apex.dev", "password123");
    ASSERT_FALSE(auth.access_token.empty());
    ASSERT_FALSE(auth.refresh_token.empty());

    // 2. Wait for Access Token to expire
    //    E2E test config should use short-lived tokens (e.g., 1-2 seconds)
    std::this_thread::sleep_for(std::chrono::seconds{2});

    // 3. Try authenticated request with expired token -> JWT_EXPIRED
    authenticate(client, auth.access_token);
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = client.recv();
        // Gateway system error: JWT_EXPIRED
        EXPECT_LT(resp.msg_id, 1000u)
            << "Expected system error for expired JWT";
    }

    // 4. Refresh Token renewal (msg_id 1004 = RefreshTokenRequest)
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto token_off = fbb.CreateString(auth.refresh_token);
        auto start = fbb.StartTable();
        fbb.AddOffset(4, token_off);  // refresh_token field
        auto loc = fbb.EndTable(start);
        fbb.Finish(flatbuffers::Offset<void>(loc));
        client.send(1004, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = client.recv();
        EXPECT_EQ(resp.msg_id, 1005u) << "Expected RefreshTokenResponse";

        // Parse new Access Token from RefreshTokenResponse
        if (!resp.payload.empty()) {
            auto* root = flatbuffers::GetRoot<flatbuffers::Table>(
                resp.payload.data());
            if (root) {
                auto* new_token = root->GetPointer<
                    const flatbuffers::String*>(6);  // access_token field
                ASSERT_NE(new_token, nullptr);
                EXPECT_FALSE(new_token->str().empty());

                // 5. Re-authenticate with new token and verify success
                authenticate(client, new_token->str());

                flatbuffers::FlatBufferBuilder fbb2(128);
                auto req2 = chat_fbs::CreateListRoomsRequest(fbb2, 0, 20);
                fbb2.Finish(req2);
                client.send(2007, fbb2.GetBufferPointer(), fbb2.GetSize());

                auto resp2 = client.recv();
                EXPECT_EQ(resp2.msg_id, 2008u)
                    << "Request with refreshed token should succeed";
            }
        }
    }

    client.close();
}

} // namespace apex::e2e
