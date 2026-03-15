#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

namespace apex::shared::protocols::tcp {

enum class ParseError : uint8_t {
    InsufficientData,
    UnsupportedVersion,
    BodyTooLarge,
};

/// Fixed 10-byte wire header for all messages.
/// Network byte order (big-endian) on the wire.
///
/// NOTE: sizeof(WireHeader) != SIZE due to struct padding.
/// Always use WireHeader::SIZE for wire protocol calculations.
/// serialize()/parse() handle field-by-field encoding.
///
/// Layout:
///   [0]     version    (uint8_t)
///   [1..2]  msg_id     (uint16_t, big-endian)
///   [3..6]  body_size  (uint32_t, big-endian)
///   [7..8]  flags      (uint16_t, big-endian)
///   [9]     reserved   (uint8_t, must be 0)
struct WireHeader {
    static_assert(sizeof(size_t) >= 8,
                  "WireHeader requires 64-bit size_t for safe frame_size() computation");
    static constexpr size_t SIZE = 10;
    static constexpr uint8_t CURRENT_VERSION = 1;
    static constexpr uint32_t MAX_BODY_SIZE = 16 * 1024 * 1024;  // 16 MB

    // Field offsets within the wire format
    static constexpr size_t OFF_VERSION   = 0;
    static constexpr size_t OFF_MSG_ID    = 1;
    static constexpr size_t OFF_BODY_SIZE = 3;
    static constexpr size_t OFF_FLAGS     = 7;
    static constexpr size_t OFF_RESERVED  = 9;

    uint8_t version{CURRENT_VERSION};
    uint16_t msg_id{0};
    uint32_t body_size{0};
    uint16_t flags{0};
    uint8_t reserved{0};

    [[nodiscard]] static std::expected<WireHeader, ParseError>
    parse(std::span<const uint8_t> data);

    void serialize(std::span<uint8_t, SIZE> out) const;
    [[nodiscard]] std::array<uint8_t, SIZE> serialize() const;

    [[nodiscard]] size_t frame_size() const noexcept {
        return SIZE + body_size;
    }
};

namespace wire_flags {
    constexpr uint16_t NONE = 0x0000;
    constexpr uint16_t COMPRESSED = 0x0001;
    constexpr uint16_t HEARTBEAT = 0x0002;
    constexpr uint16_t ERROR_RESPONSE = 0x0004;
    constexpr uint16_t REQUIRE_AUTH_CHECK = 0x0008;
} // namespace wire_flags

} // namespace apex::shared::protocols::tcp
