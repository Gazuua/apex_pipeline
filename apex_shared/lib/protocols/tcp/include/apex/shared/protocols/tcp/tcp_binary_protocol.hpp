// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/frame_codec.hpp>
#include <apex/core/protocol.hpp>

namespace apex::shared::protocols::tcp
{

/// TCP 바이너리 프로토콜 — Protocol concept 구현.
/// FrameCodec를 래핑하여 프레임 디코딩을 수행한다.
struct TcpBinaryProtocol
{
    struct Config
    {
        uint32_t max_frame_size = 64 * 1024;
    };

    using Frame = ::apex::core::Frame;

    [[nodiscard]] static apex::core::Result<Frame> try_decode(apex::core::RingBuffer& buf)
    {
        return apex::core::FrameCodec::try_decode(buf);
    }

    static void consume_frame(apex::core::RingBuffer& buf, const Frame& frame)
    {
        apex::core::FrameCodec::consume_frame(buf, frame);
    }
};

// Concept 검증
static_assert(apex::core::Protocol<TcpBinaryProtocol>);

} // namespace apex::shared::protocols::tcp
