#pragma once

/// Core 테스트용 Mock 객체 모음.
/// 실 ConnectionHandler<P>는 concrete SessionManager/MessageDispatcher를 사용하므로
/// 이 파일의 mock은 보조적 테스트 또는 향후 확장을 위해 제공된다.

#include <apex/core/error_code.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/protocol.hpp>
#include <apex/core/result.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace apex::test
{

/// Mock Protocol — Protocol concept을 만족하는 최소 구현.
/// ConnectionHandler<MockProtocol> 테스트에서 TcpBinaryProtocol 대체용.
/// 내부적으로 TcpBinaryProtocol과 동일한 FrameCodec를 사용한다.
struct MockProtocol
{
    struct Config
    {
        uint32_t max_frame_size = 64 * 1024;
    };

    using Frame = apex::core::Frame;

    [[nodiscard]] static apex::core::Result<Frame> try_decode(apex::core::RingBuffer& buf)
    {
        auto result = apex::core::FrameCodec::try_decode(buf);
        if (result.has_value())
            return std::move(*result);
        switch (result.error())
        {
            case apex::core::FrameError::InsufficientData:
                return std::unexpected(apex::core::ErrorCode::InsufficientData);
            case apex::core::FrameError::HeaderParseError:
                return std::unexpected(apex::core::ErrorCode::InvalidMessage);
            case apex::core::FrameError::BodyTooLarge:
                return std::unexpected(apex::core::ErrorCode::InvalidMessage);
        }
        return std::unexpected(apex::core::ErrorCode::Unknown);
    }

    static void consume_frame(apex::core::RingBuffer& buf, const Frame& frame)
    {
        apex::core::FrameCodec::consume_frame(buf, frame);
    }
};

static_assert(apex::core::Protocol<MockProtocol>);

/// 테스트 프레임 빌드 헬퍼 — WireHeader + payload를 직렬화된 바이트로 반환.
inline std::vector<uint8_t> build_test_frame(uint32_t msg_id, std::span<const uint8_t> payload)
{
    apex::core::WireHeader header{
        .msg_id = msg_id,
        .body_size = static_cast<uint32_t>(payload.size()),
    };
    auto hdr_bytes = header.serialize();
    std::vector<uint8_t> frame(hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// 빈 payload 프레임 빌드 헬퍼.
inline std::vector<uint8_t> build_test_frame(uint32_t msg_id)
{
    return build_test_frame(msg_id, {});
}

} // namespace apex::test
