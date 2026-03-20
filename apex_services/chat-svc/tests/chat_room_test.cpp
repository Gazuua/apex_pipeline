// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <chat_room_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

namespace
{

namespace fbs = apex::chat_svc::fbs;

TEST(ChatRoomTest, CreateRoomRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name = fbb.CreateString("test-room");
    auto req = fbs::CreateCreateRoomRequest(fbb, name, 50);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::CreateRoomRequest>(fbb.GetBufferPointer());
    EXPECT_STREQ(parsed->room_name()->c_str(), "test-room");
    EXPECT_EQ(parsed->max_members(), 50u);
}

TEST(ChatRoomTest, CreateRoomResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name = fbb.CreateString("my-room");
    auto resp = fbs::CreateCreateRoomResponse(fbb, fbs::ChatRoomError_NONE, 42, name);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::CreateRoomResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatRoomError_NONE);
    EXPECT_EQ(parsed->room_id(), 42u);
    EXPECT_STREQ(parsed->room_name()->c_str(), "my-room");
}

TEST(ChatRoomTest, CreateRoomResponseError)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateCreateRoomResponse(fbb, fbs::ChatRoomError_ROOM_NAME_EMPTY);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::CreateRoomResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatRoomError_ROOM_NAME_EMPTY);
    EXPECT_EQ(parsed->room_id(), 0u);
}

TEST(ChatRoomTest, JoinRoomRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto req = fbs::CreateJoinRoomRequest(fbb, 42);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::JoinRoomRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->room_id(), 42u);
}

TEST(ChatRoomTest, JoinRoomResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name = fbb.CreateString("General");
    auto resp = fbs::CreateJoinRoomResponse(fbb, fbs::ChatRoomError_NONE, 42, name, 10);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::JoinRoomResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatRoomError_NONE);
    EXPECT_EQ(parsed->room_id(), 42u);
    EXPECT_STREQ(parsed->room_name()->c_str(), "General");
    EXPECT_EQ(parsed->member_count(), 10u);
}

TEST(ChatRoomTest, LeaveRoomRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto req = fbs::CreateLeaveRoomRequest(fbb, 42);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::LeaveRoomRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->room_id(), 42u);
}

TEST(ChatRoomTest, LeaveRoomResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateLeaveRoomResponse(fbb, fbs::ChatRoomError_NONE, 42);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::LeaveRoomResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatRoomError_NONE);
    EXPECT_EQ(parsed->room_id(), 42u);
}

TEST(ChatRoomTest, ListRoomsRequestSchema)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto req = fbs::CreateListRoomsRequest(fbb, 20, 10);
    fbb.Finish(req);

    auto* parsed = flatbuffers::GetRoot<fbs::ListRoomsRequest>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->offset(), 20u);
    EXPECT_EQ(parsed->limit(), 10u);
}

TEST(ChatRoomTest, ListRoomsResponseSchema)
{
    flatbuffers::FlatBufferBuilder fbb(512);

    std::vector<flatbuffers::Offset<fbs::RoomInfo>> rooms;
    auto name1 = fbb.CreateString("Room A");
    rooms.push_back(fbs::CreateRoomInfo(fbb, 1, name1, 5, 100, 1001));
    auto name2 = fbb.CreateString("Room B");
    rooms.push_back(fbs::CreateRoomInfo(fbb, 2, name2, 10, 50, 1002));

    auto rooms_off = fbb.CreateVector(rooms);
    auto resp = fbs::CreateListRoomsResponse(fbb, fbs::ChatRoomError_NONE, rooms_off, 2);
    fbb.Finish(resp);

    auto* parsed = flatbuffers::GetRoot<fbs::ListRoomsResponse>(fbb.GetBufferPointer());
    EXPECT_EQ(parsed->error(), fbs::ChatRoomError_NONE);
    ASSERT_NE(parsed->rooms(), nullptr);
    EXPECT_EQ(parsed->rooms()->size(), 2u);
    EXPECT_EQ(parsed->total_count(), 2u);

    auto* room0 = parsed->rooms()->Get(0);
    EXPECT_EQ(room0->room_id(), 1u);
    EXPECT_STREQ(room0->room_name()->c_str(), "Room A");
    EXPECT_EQ(room0->member_count(), 5u);
    EXPECT_EQ(room0->max_members(), 100u);
    EXPECT_EQ(room0->owner_id(), 1001u);
}

TEST(ChatRoomTest, RoomErrorEnum)
{
    // Verify all error values are accessible
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_NONE), 0);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_ROOM_NOT_FOUND), 1);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_ROOM_FULL), 2);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_ALREADY_IN_ROOM), 3);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_NOT_IN_ROOM), 4);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_ROOM_NAME_TOO_LONG), 5);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_ROOM_NAME_EMPTY), 6);
    EXPECT_EQ(static_cast<uint16_t>(fbs::ChatRoomError_PERMISSION_DENIED), 7);
}

} // namespace
