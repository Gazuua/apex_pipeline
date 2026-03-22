// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/chat_svc/chat_logic.hpp>

#include <gtest/gtest.h>

namespace apex::chat_svc::test
{

// ============================================================
// validate_message_content
// ============================================================

TEST(ChatLogic_ValidateMessage, EmptyContent)
{
    EXPECT_EQ(validate_message_content(0, 2000), MessageValidation::EMPTY);
}

TEST(ChatLogic_ValidateMessage, NormalContent)
{
    EXPECT_EQ(validate_message_content(5, 2000), MessageValidation::OK);
}

TEST(ChatLogic_ValidateMessage, ExactlyMaxLength)
{
    EXPECT_EQ(validate_message_content(2000, 2000), MessageValidation::OK);
}

TEST(ChatLogic_ValidateMessage, ExceedsMaxLength)
{
    EXPECT_EQ(validate_message_content(2001, 2000), MessageValidation::TOO_LONG);
}

TEST(ChatLogic_ValidateMessage, OneByteContent)
{
    EXPECT_EQ(validate_message_content(1, 2000), MessageValidation::OK);
}

// ============================================================
// validate_room_name
// ============================================================

TEST(ChatLogic_ValidateRoomName, EmptyName)
{
    EXPECT_EQ(validate_room_name(0, 100), RoomNameValidation::EMPTY);
}

TEST(ChatLogic_ValidateRoomName, NormalName)
{
    EXPECT_EQ(validate_room_name(10, 100), RoomNameValidation::OK);
}

TEST(ChatLogic_ValidateRoomName, ExactlyMaxLength)
{
    EXPECT_EQ(validate_room_name(100, 100), RoomNameValidation::OK);
}

TEST(ChatLogic_ValidateRoomName, ExceedsMaxLength)
{
    EXPECT_EQ(validate_room_name(101, 100), RoomNameValidation::TOO_LONG);
}

TEST(ChatLogic_ValidateRoomName, OneCharName)
{
    EXPECT_EQ(validate_room_name(1, 100), RoomNameValidation::OK);
}

// ============================================================
// interpret_join_result
// ============================================================

TEST(ChatLogic_InterpretJoin, AlreadyInRoom)
{
    EXPECT_EQ(interpret_join_result(-1), JoinRoomResult::ALREADY_IN);
}

TEST(ChatLogic_InterpretJoin, RoomFull)
{
    EXPECT_EQ(interpret_join_result(0), JoinRoomResult::ROOM_FULL);
}

TEST(ChatLogic_InterpretJoin, SuccessWithMemberCount)
{
    EXPECT_EQ(interpret_join_result(1), JoinRoomResult::JOINED);
    EXPECT_EQ(interpret_join_result(50), JoinRoomResult::JOINED);
    EXPECT_EQ(interpret_join_result(100), JoinRoomResult::JOINED);
}

TEST(ChatLogic_InterpretJoin, UnexpectedNegativeValue_TreatedAsJoined)
{
    // Lua 스크립트가 -2 이하를 반환하면 JOINED로 해석된다.
    // 현재 프로덕션 코드의 실제 동작을 문서화하는 테스트.
    EXPECT_EQ(interpret_join_result(-2), JoinRoomResult::JOINED);
    EXPECT_EQ(interpret_join_result(-100), JoinRoomResult::JOINED);
}

// ============================================================
// max_length=0 경계값
// ============================================================

TEST(ChatLogic_ValidateMessage, ZeroMaxLength)
{
    // max_length=0이면 content_size=0은 EMPTY, 그 외는 TOO_LONG
    EXPECT_EQ(validate_message_content(0, 0), MessageValidation::EMPTY);
    EXPECT_EQ(validate_message_content(1, 0), MessageValidation::TOO_LONG);
}

TEST(ChatLogic_ValidateRoomName, ZeroMaxLength)
{
    EXPECT_EQ(validate_room_name(0, 0), RoomNameValidation::EMPTY);
    EXPECT_EQ(validate_room_name(1, 0), RoomNameValidation::TOO_LONG);
}

} // namespace apex::chat_svc::test
