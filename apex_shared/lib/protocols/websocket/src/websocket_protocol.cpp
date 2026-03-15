#include <apex/shared/protocols/websocket/websocket_protocol.hpp>

#include <cstring>

namespace apex::shared::protocols::websocket {

apex::core::Result<WebSocketProtocol::Frame>
WebSocketProtocol::try_decode(apex::core::RingBuffer& buf) {
    // MVP 구현: 4바이트 길이 접두어 (little-endian) + 페이로드.
    // Beast 완전 통합 시 이 구현을 대체할 예정.
    constexpr size_t LENGTH_PREFIX_SIZE = 4;

    if (buf.readable_size() < LENGTH_PREFIX_SIZE) {
        return std::unexpected(apex::core::ErrorCode::InsufficientData);
    }

    // 길이 접두어 읽기
    auto header_span = buf.linearize(LENGTH_PREFIX_SIZE);
    uint32_t payload_size = 0;
    std::memcpy(&payload_size, header_span.data(), sizeof(uint32_t));

    // 메시지 크기 검증 — Config::max_message_size 기본값(1MB)과 일치.
    // TODO: Beast 통합 시 Config 인스턴스를 받아 런타임 설정 가능하게 변경.
    constexpr uint32_t MAX_MESSAGE_SIZE = 1 * 1024 * 1024;  // 1MB
    if (payload_size > MAX_MESSAGE_SIZE) {
        return std::unexpected(apex::core::ErrorCode::InvalidMessage);
    }

    size_t total_size = LENGTH_PREFIX_SIZE + payload_size;
    if (buf.readable_size() < total_size) {
        return std::unexpected(apex::core::ErrorCode::InsufficientData);
    }

    // 페이로드 추출 (복사 — Frame이 vector 소유)
    auto frame_span = buf.linearize(total_size);
    Frame frame;
    frame.payload.assign(
        frame_span.data() + LENGTH_PREFIX_SIZE,
        frame_span.data() + total_size);
    frame.is_binary = true;
    frame.is_text = false;

    return frame;
}

void WebSocketProtocol::consume_frame(
    apex::core::RingBuffer& buf, const Frame& frame)
{
    constexpr size_t LENGTH_PREFIX_SIZE = 4;
    buf.consume(LENGTH_PREFIX_SIZE + frame.payload.size());
}

} // namespace apex::shared::protocols::websocket
