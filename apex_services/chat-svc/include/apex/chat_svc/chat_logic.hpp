// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstddef>
#include <cstdint>

namespace apex::chat_svc
{

/// 메시지 콘텐츠 검증 결과.
enum class MessageValidation : uint8_t
{
    OK,
    EMPTY,
    TOO_LONG
};

/// 방 이름 검증 결과.
enum class RoomNameValidation : uint8_t
{
    OK,
    EMPTY,
    TOO_LONG
};

/// Redis Lua 스크립트 join 결과 해석.
enum class JoinRoomResult : uint8_t
{
    JOINED,     ///< 정상 입장 (Lua > 0)
    ALREADY_IN, ///< 이미 방에 있음 (Lua == -1)
    ROOM_FULL   ///< 방 정원 초과 (Lua == 0)
};

/// 메시지 콘텐츠 검증 (순수 함수).
[[nodiscard]] constexpr MessageValidation validate_message_content(size_t content_size, size_t max_length) noexcept
{
    if (content_size == 0)
        return MessageValidation::EMPTY;
    if (content_size > max_length)
        return MessageValidation::TOO_LONG;
    return MessageValidation::OK;
}

/// 방 이름 검증 (순수 함수).
[[nodiscard]] constexpr RoomNameValidation validate_room_name(size_t name_size, size_t max_length) noexcept
{
    if (name_size == 0)
        return RoomNameValidation::EMPTY;
    if (name_size > max_length)
        return RoomNameValidation::TOO_LONG;
    return RoomNameValidation::OK;
}

/// Redis Lua EVAL join 결과 해석 (순수 함수).
/// @param lua_result  EVAL 반환 정수: -1 = already in, 0 = full, >0 = 현재 멤버 수 (성공).
[[nodiscard]] constexpr JoinRoomResult interpret_join_result(int64_t lua_result) noexcept
{
    if (lua_result == -1)
        return JoinRoomResult::ALREADY_IN;
    if (lua_result == 0)
        return JoinRoomResult::ROOM_FULL;
    return JoinRoomResult::JOINED;
}

} // namespace apex::chat_svc
