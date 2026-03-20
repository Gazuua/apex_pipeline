// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <chat_message_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

namespace
{

namespace fbs = apex::chat_svc::fbs;

// ============================================================
// Send Message (Task 4)
// ============================================================

TEST(ChatMessageTest, SendMessageRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto content = fbb.CreateString("Hello, world!");
    auto req = fbs::CreateSendMessageRequest(fbb, 42, content);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::SendMessageRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->room_id(), 42u);
    EXPECT_STREQ(parsed->content()->c_str(), "Hello, world!");
}

TEST(ChatMessageTest, SendMessageResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateSendMessageResponse(fbb, fbs::ChatMessageError_NONE, 99999, 1710000000000ULL);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::SendMessageResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_NONE);
    EXPECT_EQ(parsed->message_id(), 99999u);
    EXPECT_EQ(parsed->timestamp(), 1710000000000ULL);
}

TEST(ChatMessageTest, SendMessageResponseError)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateSendMessageResponse(fbb, fbs::ChatMessageError_NOT_IN_ROOM);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::SendMessageResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_NOT_IN_ROOM);
}

TEST(ChatMessageTest, ChatMessageBroadcastSchema)
{
    flatbuffers::FlatBufferBuilder fbb(512);
    auto sender = fbb.CreateString("Alice");
    auto content = fbb.CreateString("Hi everyone!");
    auto channel = fbb.CreateString("pub:chat:room:42");
    auto msg = fbs::CreateChatMessage(fbb, 42, 1001, sender, content, 12345, 1710000000000ULL, channel);
    fbb.Finish(msg);

    auto* parsed = flatbuffers::GetRoot<fbs::ChatMessage>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->room_id(), 42u);
    EXPECT_EQ(parsed->sender_id(), 1001u);
    EXPECT_STREQ(parsed->sender_name()->c_str(), "Alice");
    EXPECT_STREQ(parsed->content()->c_str(), "Hi everyone!");
    EXPECT_EQ(parsed->message_id(), 12345u);
    EXPECT_EQ(parsed->timestamp(), 1710000000000ULL);
    EXPECT_STREQ(parsed->channel()->c_str(), "pub:chat:room:42");
}

TEST(ChatMessageTest, ChatMessageVerification)
{
    flatbuffers::FlatBufferBuilder fbb(512);
    auto sender = fbb.CreateString("Bob");
    auto content = fbb.CreateString("Test");
    auto channel = fbb.CreateString("pub:chat:room:1");
    auto msg = fbs::CreateChatMessage(fbb, 1, 100, sender, content, 1, 1710000000000ULL, channel);
    fbb.Finish(msg);

    flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
    EXPECT_TRUE(verifier.VerifyBuffer<fbs::ChatMessage>());
}

// ============================================================
// Whisper (Task 5)
// ============================================================

TEST(ChatMessageTest, WhisperRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto content = fbb.CreateString("Psst, secret message");
    auto req = fbs::CreateWhisperRequest(fbb, 2001, content);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::WhisperRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->target_user_id(), 2001u);
    EXPECT_STREQ(parsed->content()->c_str(), "Psst, secret message");
}

TEST(ChatMessageTest, WhisperResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateWhisperResponse(fbb, fbs::ChatMessageError_NONE, 1710000000000ULL);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::WhisperResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_NONE);
    EXPECT_EQ(parsed->timestamp(), 1710000000000ULL);
}

TEST(ChatMessageTest, WhisperResponseTargetOffline)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateWhisperResponse(fbb, fbs::ChatMessageError_TARGET_OFFLINE);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::WhisperResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_TARGET_OFFLINE);
}

TEST(ChatMessageTest, WhisperMessageSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto sender = fbb.CreateString("Bob");
    auto content = fbb.CreateString("Whisper to you");
    auto msg = fbs::CreateWhisperMessage(fbb, 1001, sender, content, 1710000000000ULL);
    fbb.Finish(msg);

    auto* parsed = flatbuffers::GetRoot<fbs::WhisperMessage>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->sender_id(), 1001u);
    EXPECT_STREQ(parsed->sender_name()->c_str(), "Bob");
    EXPECT_STREQ(parsed->content()->c_str(), "Whisper to you");
    EXPECT_EQ(parsed->timestamp(), 1710000000000ULL);
}

// ============================================================
// Chat History (Task 6)
// ============================================================

TEST(ChatMessageTest, ChatHistoryRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto req = fbs::CreateChatHistoryRequest(fbb, 42, 500, 25);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::ChatHistoryRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->room_id(), 42u);
    EXPECT_EQ(parsed->before_message_id(), 500u);
    EXPECT_EQ(parsed->limit(), 25u);
}

TEST(ChatMessageTest, ChatHistoryRequestDefaults)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto req = fbs::CreateChatHistoryRequest(fbb, 42);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::ChatHistoryRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->room_id(), 42u);
    EXPECT_EQ(parsed->before_message_id(), 0u); // default
    EXPECT_EQ(parsed->limit(), 50u);            // default
}

TEST(ChatMessageTest, ChatHistoryResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(1024);

    std::vector<flatbuffers::Offset<fbs::HistoryMessage>> msgs;
    auto sender1 = fbb.CreateString("Alice");
    auto content1 = fbb.CreateString("Old message");
    msgs.push_back(fbs::CreateHistoryMessage(fbb, 100, 1001, sender1, content1, 1710000000000ULL));

    auto sender2 = fbb.CreateString("Bob");
    auto content2 = fbb.CreateString("Even older message");
    msgs.push_back(fbs::CreateHistoryMessage(fbb, 99, 1002, sender2, content2, 1709999999000ULL));

    auto msgs_off = fbb.CreateVector(msgs);
    auto resp = fbs::CreateChatHistoryResponse(fbb, fbs::ChatMessageError_NONE, msgs_off, true);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::ChatHistoryResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_NONE);
    ASSERT_NE(parsed->messages(), nullptr);
    EXPECT_EQ(parsed->messages()->size(), 2u);
    EXPECT_TRUE(parsed->has_more());

    auto* msg0 = parsed->messages()->Get(0);
    EXPECT_EQ(msg0->message_id(), 100u);
    EXPECT_EQ(msg0->sender_id(), 1001u);
    EXPECT_STREQ(msg0->sender_name()->c_str(), "Alice");
    EXPECT_STREQ(msg0->content()->c_str(), "Old message");
}

TEST(ChatMessageTest, ChatHistoryResponseEmpty)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateChatHistoryResponse(fbb, fbs::ChatMessageError_NOT_IN_ROOM);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::ChatHistoryResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_NOT_IN_ROOM);
}

// ============================================================
// Global Broadcast (Task 7)
// ============================================================

TEST(ChatMessageTest, GlobalBroadcastRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto content = fbb.CreateString("Server maintenance in 10 minutes");
    auto req = fbs::CreateGlobalBroadcastRequest(fbb, content);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::GlobalBroadcastRequest>(fbb.GetBufferPointer());
    EXPECT_STREQ(parsed->content()->c_str(), "Server maintenance in 10 minutes");
}

TEST(ChatMessageTest, GlobalBroadcastResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateGlobalBroadcastResponse(fbb, fbs::ChatMessageError_NONE, 1710000000000ULL);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::GlobalBroadcastResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatMessageError_NONE);
    EXPECT_EQ(parsed->timestamp(), 1710000000000ULL);
}

TEST(ChatMessageTest, GlobalChatMessageSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto sender = fbb.CreateString("Admin");
    auto content = fbb.CreateString("Server maintenance in 10 minutes");
    auto channel = fbb.CreateString("pub:global:chat");
    auto msg = fbs::CreateGlobalChatMessage(fbb, 0, sender, content, 1710000000000ULL, channel);
    fbb.Finish(msg);

    auto* parsed = flatbuffers::GetRoot<fbs::GlobalChatMessage>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->sender_id(), 0u);
    EXPECT_STREQ(parsed->sender_name()->c_str(), "Admin");
    EXPECT_STREQ(parsed->content()->c_str(), "Server maintenance in 10 minutes");
    EXPECT_EQ(parsed->timestamp(), 1710000000000ULL);
    EXPECT_STREQ(parsed->channel()->c_str(), "pub:global:chat");
}

TEST(ChatMessageTest, MessageErrorEnum)
{
    // Verify all error values are accessible
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_NONE), 0);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_NOT_IN_ROOM), 1);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_MESSAGE_TOO_LONG), 2);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_EMPTY_MESSAGE), 3);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_TARGET_NOT_FOUND), 4);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_TARGET_OFFLINE), 5);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_RATE_LIMITED), 6);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatMessageError_PERMISSION_DENIED), 7);
}

} // namespace
