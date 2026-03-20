// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "e2e_test_fixture.hpp"

#include <chat_message_generated.h>
#include <chat_room_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <thread>

namespace apex::e2e
{

namespace chat_fbs = apex::chat_svc::fbs;

class ChatE2ETest : public E2ETestFixture
{};

/// Scenario 2: Room join -> message send -> broadcast receive
///
/// Full pipeline:
///   Alice creates room -> Bob joins -> Alice sends message
///   -> Chat Service -> Redis PUBLISH -> Gateway SUBSCRIBE -> fan-out to Bob
TEST_F(ChatE2ETest, RoomMessageBroadcast)
{
    // --- Alice login ---
    TcpClient alice(io_ctx_, config_);
    alice.connect();
    auto alice_auth = login(alice, "alice@apex.dev", "password123");
    authenticate(alice, alice_auth.access_token);

    // --- Bob login ---
    TcpClient bob(io_ctx_, config_);
    bob.connect();
    auto bob_auth = login(bob, "bob@apex.dev", "password123");
    authenticate(bob, bob_auth.access_token);

    // 1. Alice creates a room
    uint64_t room_id = 0;
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto name = fbb.CreateString("E2E Test Room");
        auto req = chat_fbs::CreateCreateRoomRequest(fbb, name, 50);
        fbb.Finish(req);
        alice.send(2001, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = alice.recv();
        ASSERT_EQ(resp.msg_id, 2002u);
        auto* create_resp = flatbuffers::GetRoot<chat_fbs::CreateRoomResponse>(resp.payload.data());
        ASSERT_EQ(create_resp->error(), chat_fbs::ChatRoomError_NONE);
        room_id = create_resp->room_id();
        ASSERT_GT(room_id, 0u);
    }

    // 2. Bob joins the room
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateJoinRoomRequest(fbb, room_id);
        fbb.Finish(req);
        bob.send(2003, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = bob.recv();
        ASSERT_EQ(resp.msg_id, 2004u);
        auto* join_resp = flatbuffers::GetRoot<chat_fbs::JoinRoomResponse>(resp.payload.data());
        ASSERT_EQ(join_resp->error(), chat_fbs::ChatRoomError_NONE);
        EXPECT_EQ(join_resp->member_count(), 2u); // Alice + Bob
    }

    // 2b. Subscribe Bob to the room's Pub/Sub channel for broadcast delivery
    subscribe_channel(bob, "pub:chat:room:" + std::to_string(room_id));
    // PubSubListener uses select() with 1-second timeout to poll for pending
    // subscriptions. Wait long enough for the subscription to be applied on
    // the Redis side before sending the message.
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});

    // 3. Alice sends a message
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto content = fbb.CreateString("Hello from Alice!");
        auto req = chat_fbs::CreateSendMessageRequest(fbb, room_id, content);
        fbb.Finish(req);
        alice.send(2011, fbb.GetBufferPointer(), fbb.GetSize());

        // Alice receives SendMessageResponse
        auto resp = alice.recv();
        EXPECT_EQ(resp.msg_id, 2012u);
    }

    // 4. Bob receives broadcast message (Redis Pub/Sub -> Gateway -> Bob)
    {
        auto msg = bob.recv(std::chrono::seconds{5});
        EXPECT_EQ(msg.msg_id, 2013u); // ChatMessage broadcast

        auto* chat_msg = flatbuffers::GetRoot<chat_fbs::ChatMessage>(msg.payload.data());
        EXPECT_EQ(chat_msg->room_id(), room_id);
        ASSERT_NE(chat_msg->sender_name(), nullptr);
        // sender_name is set by Chat Service from user data
        ASSERT_NE(chat_msg->content(), nullptr);
        EXPECT_STREQ(chat_msg->content()->c_str(), "Hello from Alice!");
    }

    // 5. Bob leaves room
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateLeaveRoomRequest(fbb, room_id);
        fbb.Finish(req);
        bob.send(2005, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = bob.recv();
        EXPECT_EQ(resp.msg_id, 2006u);
    }

    alice.close();
    bob.close();
}

/// Supplementary: list rooms after creation
TEST_F(ChatE2ETest, ListRooms)
{
    TcpClient client(io_ctx_, config_);
    client.connect();
    auto auth = login(client, "alice@apex.dev", "password123");
    authenticate(client, auth.access_token);

    // Create a room
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto name = fbb.CreateString("List Test Room");
        auto req = chat_fbs::CreateCreateRoomRequest(fbb, name, 10);
        fbb.Finish(req);
        client.send(2001, fbb.GetBufferPointer(), fbb.GetSize());
        client.recv(); // CreateRoomResponse
    }

    // List rooms
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
        fbb.Finish(req);
        client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = client.recv();
        ASSERT_EQ(resp.msg_id, 2008u);
        auto* list_resp = flatbuffers::GetRoot<chat_fbs::ListRoomsResponse>(resp.payload.data());
        EXPECT_GT(list_resp->total_count(), 0u);
        ASSERT_NE(list_resp->rooms(), nullptr);
        EXPECT_GT(list_resp->rooms()->size(), 0u);
    }

    client.close();
}

/// Scenario 3: Global broadcast -> all connected users receive
///
/// Alice sends global broadcast -> both Alice and Bob receive GlobalChatMessage
TEST_F(ChatE2ETest, GlobalBroadcast)
{
    // Alice and Bob login
    TcpClient alice(io_ctx_, config_);
    alice.connect();
    auto alice_auth = login(alice, "alice@apex.dev", "password123");
    authenticate(alice, alice_auth.access_token);

    TcpClient bob(io_ctx_, config_);
    bob.connect();
    auto bob_auth = login(bob, "bob@apex.dev", "password123");
    authenticate(bob, bob_auth.access_token);

    // Alice sends global broadcast (msg_id 2041)
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto content = fbb.CreateString("Server maintenance at 03:00 UTC");
        auto req = chat_fbs::CreateGlobalBroadcastRequest(fbb, content);
        fbb.Finish(req);
        alice.send(2041, fbb.GetBufferPointer(), fbb.GetSize());
    }

    // Alice receives both GlobalBroadcastResponse (2042) and
    // GlobalChatMessage (2043) in non-deterministic order.
    // Redis Pub/Sub delivery can be faster than the Kafka response round-trip,
    // so the broadcast may arrive before the response.
    {
        auto msg1 = alice.recv(std::chrono::seconds{5});
        auto msg2 = alice.recv(std::chrono::seconds{5});

        bool got_response = (msg1.msg_id == 2042 || msg2.msg_id == 2042);
        bool got_broadcast = (msg1.msg_id == 2043 || msg2.msg_id == 2043);

        EXPECT_TRUE(got_response) << "Expected GlobalBroadcastResponse (2042), got " << msg1.msg_id << " and "
                                  << msg2.msg_id;
        EXPECT_TRUE(got_broadcast) << "Expected GlobalChatMessage (2043), got " << msg1.msg_id << " and "
                                   << msg2.msg_id;

        // Verify broadcast content regardless of arrival order
        auto& broadcast = (msg1.msg_id == 2043) ? msg1 : msg2;
        auto* global_msg = flatbuffers::GetRoot<chat_fbs::GlobalChatMessage>(broadcast.payload.data());
        ASSERT_NE(global_msg->content(), nullptr);
        EXPECT_STREQ(global_msg->content()->c_str(), "Server maintenance at 03:00 UTC");
        ASSERT_NE(global_msg->channel(), nullptr);
        EXPECT_STREQ(global_msg->channel()->c_str(), "pub:global:chat");
    }

    // Bob receives GlobalChatMessage (pub:global:chat channel)
    {
        auto msg = bob.recv(std::chrono::seconds{5});
        EXPECT_EQ(msg.msg_id, 2043u);
    }

    alice.close();
    bob.close();
}

} // namespace apex::e2e
