#pragma once

#include <apex/core/protocol.hpp>
#include <apex/shared/protocols/tcp/frame_codec.hpp>

namespace apex::shared::protocols::tcp
{

/// TCP 바이너리 프로토콜 — Protocol concept 구현.
/// FrameCodec를 래핑하여 FrameError → ErrorCode 변환을 수행한다.
struct TcpBinaryProtocol
{
    struct Config
    {
        uint32_t max_frame_size = 64 * 1024;
    };

    using Frame = ::apex::shared::protocols::tcp::Frame;

    [[nodiscard]] static apex::core::Result<Frame> try_decode(apex::core::RingBuffer& buf)
    {
        auto result = FrameCodec::try_decode(buf);
        if (result.has_value())
            return std::move(*result);
        // FrameError -> ErrorCode 변환
        switch (result.error())
        {
            case FrameError::InsufficientData:
                return std::unexpected(apex::core::ErrorCode::InsufficientData);
            case FrameError::HeaderParseError:
                return std::unexpected(apex::core::ErrorCode::InvalidMessage);
            case FrameError::BodyTooLarge:
                return std::unexpected(apex::core::ErrorCode::InvalidMessage);
        }
        return std::unexpected(apex::core::ErrorCode::Unknown);
    }

    static void consume_frame(apex::core::RingBuffer& buf, const Frame& frame)
    {
        FrameCodec::consume_frame(buf, frame);
    }
};

// Concept 검증
static_assert(apex::core::Protocol<TcpBinaryProtocol>);

} // namespace apex::shared::protocols::tcp
