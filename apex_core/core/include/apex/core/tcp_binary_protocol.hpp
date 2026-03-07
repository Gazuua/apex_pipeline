#pragma once

#include <apex/core/protocol_base.hpp>
#include <apex/core/frame_codec.hpp>

namespace apex::core {

/// TCP 바이너리 프로토콜. 기존 FrameCodec를 래핑한다.
struct TcpBinaryProtocol : ProtocolBase<TcpBinaryProtocol> {
    using FrameType = Frame;

    [[nodiscard]] static std::expected<Frame, FrameError>
    try_decode_impl(RingBuffer& buf) {
        return FrameCodec::try_decode(buf);
    }

    static void consume_frame_impl(RingBuffer& buf, const Frame& frame) {
        FrameCodec::consume_frame(buf, frame);
    }
};

} // namespace apex::core
