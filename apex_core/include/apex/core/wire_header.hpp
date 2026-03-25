// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace apex::core
{

/// Fixed 12-byte wire header for all messages (v2).
/// Network byte order (big-endian) on the wire.
///
/// NOTE: sizeof(WireHeader) != SIZE due to struct padding.
/// Always use WireHeader::SIZE for wire protocol calculations.
/// serialize()/parse() handle field-by-field encoding.
///
/// Layout (v2):
///   [0]       version    (uint8_t)
///   [1]       flags      (uint8_t)
///   [2..5]    msg_id     (uint32_t, big-endian)
///   [6..9]    body_size  (uint32_t, big-endian)
///   [10..11]  reserved   (uint16_t, must be 0)
struct WireHeader
{
    static_assert(sizeof(size_t) >= 8, "WireHeader requires 64-bit size_t for safe frame_size() computation");
    static constexpr size_t SIZE = 12;
    static constexpr uint8_t CURRENT_VERSION = 2;
    static constexpr uint32_t MAX_BODY_SIZE = 16 * 1024 * 1024; // 16 MB

    // Field offsets within the wire format
    static constexpr size_t OFF_VERSION = 0;
    static constexpr size_t OFF_FLAGS = 1;
    static constexpr size_t OFF_MSG_ID = 2;
    static constexpr size_t OFF_BODY_SIZE = 6;
    static constexpr size_t OFF_RESERVED = 10;

    uint8_t version{CURRENT_VERSION};
    uint8_t flags{0};
    uint32_t msg_id{0};
    uint32_t body_size{0};
    uint16_t reserved{0};

    [[nodiscard]] static Result<WireHeader> parse(std::span<const uint8_t> data);

    void serialize(std::span<uint8_t, SIZE> out) const;
    [[nodiscard]] std::array<uint8_t, SIZE> serialize() const;

    [[nodiscard]] size_t frame_size() const noexcept
    {
        return SIZE + body_size;
    }
};

namespace wire_flags
{
constexpr uint8_t NONE = 0x00;
constexpr uint8_t COMPRESSED = 0x01;
constexpr uint8_t HEARTBEAT = 0x02;
constexpr uint8_t ERROR_RESPONSE = 0x04;
constexpr uint8_t REQUIRE_AUTH_CHECK = 0x08;
} // namespace wire_flags

} // namespace apex::core
