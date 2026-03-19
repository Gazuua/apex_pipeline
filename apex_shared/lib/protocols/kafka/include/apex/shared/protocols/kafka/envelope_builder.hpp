#pragma once

#include <apex/core/bump_allocator.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::protocols::kafka
{

/// Kafka Envelope를 빌더 패턴으로 구성하는 클래스.
///
/// 기존 build_full_envelope()을 빌더 패턴으로 감싸며,
/// 핫패스용 build_into(BumpAllocator&) 메서드를 추가로 제공한다.
///
/// 바이트 레이아웃은 build_full_envelope()과 완전히 동일:
///   [RoutingHeader 8B] [MetadataPrefix 40B] [ReplyTopic? 2+NB] [Payload]
///
/// 사용 예시:
///   auto bytes = EnvelopeBuilder{}
///       .routing(msg_id, flags)
///       .metadata(core_id, corr_id, source_id, session_id, user_id)
///       .reply_topic("chat.responses")
///       .payload(data)
///       .build();
class EnvelopeBuilder
{
  public:
    EnvelopeBuilder() = default;

    /// RoutingHeader 파라미터 설정.
    /// @param msg_id  메시지 ID
    /// @param flags   라우팅 플래그 (기본값 0). HAS_REPLY_TOPIC은 reply_topic()
    ///                설정 여부에 따라 build 시점에 자동 처리됨.
    EnvelopeBuilder& routing(uint32_t msg_id, uint16_t flags = 0);

    /// MetadataPrefix 파라미터 설정.
    /// @param core_id    코어 ID
    /// @param corr_id    상관 ID (요청-응답 매핑용)
    /// @param source_id  소스 서비스 ID (source_ids::GATEWAY 등)
    /// @param session_id 세션 ID
    /// @param user_id    사용자 ID (JWT에서 추출)
    EnvelopeBuilder& metadata(uint16_t core_id, uint64_t corr_id, uint16_t source_id, uint64_t session_id,
                              uint64_t user_id);

    /// Reply-To 토픽 설정. 비어있으면 ReplyTopic 섹션이 생략됨.
    EnvelopeBuilder& reply_topic(std::string_view topic);

    /// 엔벨로프에 실을 페이로드 설정.
    EnvelopeBuilder& payload(std::span<const uint8_t> data);

    /// BumpAllocator에 엔벨로프를 직접 직렬화한다 (힙 할당 없음, 핫패스용).
    ///
    /// 총 크기를 계산한 뒤 alloc에서 단 1회 할당하고 in-place 직렬화한다.
    /// alloc 용량이 부족하면 빈 span을 반환한다.
    ///
    /// @param alloc  BumpAllocator 참조 (수명이 반환된 span보다 길어야 함)
    /// @return 직렬화된 엔벨로프 바이트 span, 실패 시 빈 span
    [[nodiscard]] std::span<uint8_t> build_into(apex::core::BumpAllocator& alloc);

    /// 힙에 엔벨로프를 구성해 vector로 반환한다 (편의용).
    ///
    /// 내부적으로 build_full_envelope()과 동일한 로직을 사용한다.
    [[nodiscard]] std::vector<uint8_t> build();

  private:
    /// 총 직렬화 크기를 계산한다.
    [[nodiscard]] size_t total_size() const noexcept;

    /// buf[0..total_size()) 범위에 엔벨로프를 in-place 직렬화한다.
    void serialize_into(uint8_t* buf) const;

    // --- RoutingHeader 파라미터 ---
    uint32_t msg_id_{0};
    uint16_t flags_{0};

    // --- MetadataPrefix 파라미터 ---
    uint16_t core_id_{0};
    uint64_t corr_id_{0};
    uint16_t source_id_{0};
    uint64_t session_id_{0};
    uint64_t user_id_{0};

    // --- Reply-To 토픽 ---
    std::string reply_topic_{};

    // --- 페이로드 ---
    std::span<const uint8_t> payload_{};
};

} // namespace apex::shared::protocols::kafka
