#pragma once

#include <apex/core/ring_buffer.hpp>

namespace apex::core {

/// 프레임 기반 프로토콜의 CRTP 추상화.
/// Derived는 FrameType typedef, try_decode_impl(RingBuffer&),
/// consume_frame_impl(RingBuffer&, const FrameType&)을 제공해야 한다.
template <typename Derived>
struct ProtocolBase {
    [[nodiscard]] static auto try_decode(RingBuffer& buf) {
        return Derived::try_decode_impl(buf);
    }

    template <typename FrameT>
    static void consume_frame(RingBuffer& buf, const FrameT& frame) {
        Derived::consume_frame_impl(buf, frame);
    }
};

} // namespace apex::core
