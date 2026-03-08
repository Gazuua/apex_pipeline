#pragma once

#include <apex/core/ring_buffer.hpp>

#include <type_traits>

namespace apex::core {

/// CRTP base for frame-based protocol abstraction.
/// Derived must provide: FrameType typedef, try_decode_impl(RingBuffer&),
/// and consume_frame_impl(RingBuffer&, const FrameType&).
///
/// @note Currently only TcpBinaryProtocol exists. This CRTP base provides a
/// compile-time interface contract for future protocol variants (e.g., WebSocket, HTTP/2).
template <typename Derived>
struct ProtocolBase {
    [[nodiscard]] static auto try_decode(RingBuffer& buf) {
        return Derived::try_decode_impl(buf);
    }

    /// @tparam FrameT must be Derived::FrameType
    template <typename FrameT>
    static void consume_frame(RingBuffer& buf, const FrameT& frame) {
        // MSVC 불완전 타입 이슈로 typename Derived::FrameType 직접 사용 불가할 수 있음.
        // static_assert로 호출 시점에 타입 일치를 검증한다.
        static_assert(std::is_same_v<FrameT, typename Derived::FrameType>,
                      "FrameT must match Derived::FrameType");
        Derived::consume_frame_impl(buf, frame);
    }
};

} // namespace apex::core
