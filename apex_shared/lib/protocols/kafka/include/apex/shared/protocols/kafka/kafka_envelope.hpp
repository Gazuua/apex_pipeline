#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::protocols::kafka {

// ============================================================
// Routing Header (8바이트, 불변)
// ============================================================
// header_version(u16) | flags(u16) | msg_id(u32)

enum class EnvelopeError : uint8_t {
    InsufficientData,
    UnsupportedVersion,
};

struct RoutingHeader {
    static constexpr size_t SIZE = 8;
    static constexpr uint16_t CURRENT_VERSION = 1;

    static constexpr size_t OFF_HEADER_VERSION = 0;
    static constexpr size_t OFF_FLAGS          = 2;
    static constexpr size_t OFF_MSG_ID         = 4;

    uint16_t header_version{CURRENT_VERSION};
    uint16_t flags{0};
    uint32_t msg_id{0};

    [[nodiscard]] static std::expected<RoutingHeader, EnvelopeError>
    parse(std::span<const uint8_t> data);

    void serialize(std::span<uint8_t, SIZE> out) const;
    [[nodiscard]] std::array<uint8_t, SIZE> serialize() const;
};

// ============================================================
// Routing Flags (u16 비트맵)
// ============================================================
namespace routing_flags {
    // Bit 0: 방향
    constexpr uint16_t DIRECTION_RESPONSE = 0x0001;   // 0=요청, 1=응답
    // Bit 1: 에러
    constexpr uint16_t ERROR_BIT          = 0x0002;
    // Bit 2-3: 전달 방식
    constexpr uint16_t DELIVERY_MASK      = 0x000C;   // 2비트 마스크
    constexpr uint16_t DELIVERY_UNICAST   = 0x0000;   // 00=단일
    constexpr uint16_t DELIVERY_CHANNEL   = 0x0004;   // 01=채널
    constexpr uint16_t DELIVERY_BROADCAST = 0x0008;   // 10=전역
    // Bit 4: 압축
    constexpr uint16_t COMPRESSED         = 0x0010;
    // Bit 5: 암호화
    constexpr uint16_t ENCRYPTED          = 0x0020;
    // Bit 6-7: 우선순위
    constexpr uint16_t PRIORITY_MASK      = 0x00C0;
    constexpr uint16_t PRIORITY_NORMAL    = 0x0000;   // 00=일반
    constexpr uint16_t PRIORITY_HIGH      = 0x0040;   // 01=높음
    constexpr uint16_t PRIORITY_URGENT    = 0x0080;   // 10=긴급
    constexpr uint16_t PRIORITY_SYSTEM    = 0x00C0;   // 11=시스템
    // Bit 8-9: TTL 힌트
    constexpr uint16_t TTL_MASK           = 0x0300;
    constexpr uint16_t TTL_DEFAULT        = 0x0000;
    constexpr uint16_t TTL_SHORT          = 0x0100;
    constexpr uint16_t TTL_LONG           = 0x0200;
    constexpr uint16_t TTL_PERMANENT      = 0x0300;
    // Bit 10: 프래그먼트
    constexpr uint16_t FRAGMENT           = 0x0400;
    // Bit 11: Reply-To (아래 확장 블록에서 정의)
    // Bit 12-15: 예약
} // namespace routing_flags

// ============================================================
// Metadata Prefix (40바이트, 고정)
// ============================================================
// meta_version(u32) | core_id(u16) | corr_id(u64)
// | source_id(u16) | session_id(u64) | user_id(u64) | timestamp(u64)

struct MetadataPrefix {
    static constexpr size_t SIZE = 40;
    static constexpr uint32_t CURRENT_VERSION = 2;

    static constexpr size_t OFF_META_VERSION = 0;
    static constexpr size_t OFF_CORE_ID      = 4;
    static constexpr size_t OFF_CORR_ID      = 6;
    static constexpr size_t OFF_SOURCE_ID    = 14;
    static constexpr size_t OFF_SESSION_ID   = 16;
    static constexpr size_t OFF_USER_ID      = 24;
    static constexpr size_t OFF_TIMESTAMP    = 32;

    uint32_t meta_version{CURRENT_VERSION};
    uint16_t core_id{0};
    uint64_t corr_id{0};
    uint16_t source_id{0};     // 0=시스템/Gateway, 1=Auth, 2=Chat, ...
    uint64_t session_id{0};
    uint64_t user_id{0};       // JWT에서 추출한 사용자 ID
    uint64_t timestamp{0};     // epoch milliseconds

    [[nodiscard]] static std::expected<MetadataPrefix, EnvelopeError>
    parse(std::span<const uint8_t> data);

    void serialize(std::span<uint8_t, SIZE> out) const;
    [[nodiscard]] std::array<uint8_t, SIZE> serialize() const;
};

// ============================================================
// Full Envelope 상수
// ============================================================
/// 고정 헤더 크기 = RoutingHeader(8) + MetadataPrefix(40) = 48바이트
/// 실제 Payload 시작 오프셋은 reply_topic 존재 여부에 따라 달라짐.
/// reply_topic이 있으면: 48 + 2 + reply_topic.size()
/// reply_topic이 없으면: 48 (하위 호환)
static constexpr size_t ENVELOPE_HEADER_SIZE =
    RoutingHeader::SIZE + MetadataPrefix::SIZE;  // 48

/// Source ID 상수
namespace source_ids {
    constexpr uint16_t GATEWAY = 0;
    constexpr uint16_t AUTH    = 1;
    constexpr uint16_t CHAT    = 2;
} // namespace source_ids

// ============================================================
// Reply-To Header (가변 길이, Routing Flags HAS_REPLY_TOPIC으로 존재 표시)
// ============================================================
// reply_topic_len(u16) | reply_topic(UTF-8, reply_topic_len바이트)
//
// 레이아웃: [RoutingHeader 8B] [MetadataPrefix 40B] [ReplyTopic 2+NB] [Payload]
// HAS_REPLY_TOPIC 플래그가 없으면 ReplyTopic 섹션 없음 (하위 호환).

namespace routing_flags {
    /// Bit 11: Reply-To 토픽 존재 여부
    constexpr uint16_t HAS_REPLY_TOPIC  = 0x0800;
} // namespace routing_flags (extension)

/// Reply-To 헤더 직렬화/역직렬화 유틸
struct ReplyTopicHeader {
    /// reply_topic이 비어있으면 빈 vector 반환 (직렬화하지 않음).
    [[nodiscard]] static std::vector<uint8_t>
    serialize(std::string_view reply_topic);

    /// data에서 reply_topic을 파싱. reply_topic_len(u16 BE) + topic bytes.
    /// @return {reply_topic, 소비한 바이트 수} 또는 에러
    [[nodiscard]] static std::expected<std::pair<std::string, size_t>, EnvelopeError>
    parse(std::span<const uint8_t> data);
};

/// Envelope 전체를 빌드하는 헬퍼.
/// [RoutingHeader] [MetadataPrefix] [ReplyTopic?] [Payload]
/// HAS_REPLY_TOPIC 플래그는 reply_topic 유무에 따라 내부에서 자동 설정/해제됨.
/// 호출자가 routing.flags에 직접 설정할 필요 없음.
[[nodiscard]] std::vector<uint8_t> build_full_envelope(
    const RoutingHeader& routing,
    const MetadataPrefix& metadata,
    std::string_view reply_topic,
    std::span<const uint8_t> payload);

/// Envelope에서 Payload 시작 오프셋을 계산.
/// HAS_REPLY_TOPIC 플래그가 있으면 reply_topic 섹션을 건너뜀.
[[nodiscard]] size_t envelope_payload_offset(
    uint16_t routing_flags,
    std::span<const uint8_t> data);

/// Envelope에서 reply_topic을 추출. 없으면 빈 문자열.
[[nodiscard]] std::string extract_reply_topic(
    uint16_t routing_flags,
    std::span<const uint8_t> data);

} // namespace apex::shared::protocols::kafka
