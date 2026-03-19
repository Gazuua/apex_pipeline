#pragma once

#include <apex/core/result.hpp>
#include <apex/core/ring_buffer.hpp>

#include <concepts>

namespace apex::core
{

/// Protocol concept — core에서 정의, shared에서 구현.
/// 의존성 역전: core는 concept만, 구체 프로토콜은 shared가 제공.
///
/// 요구사항:
///   - P::Config  — 프로토콜별 설정 타입
///   - P::Frame   — 디코딩 결과 프레임 타입
///   - P::try_decode(RingBuffer&) -> Result<P::Frame>
///   - P::consume_frame(RingBuffer&, const P::Frame&) -> void
template <typename P>
concept Protocol = requires {
    typename P::Config;
    typename P::Frame;
} && requires(RingBuffer& buf, const typename P::Frame& frame) {
    { P::try_decode(buf) } -> std::same_as<Result<typename P::Frame>>;
    { P::consume_frame(buf, frame) } -> std::same_as<void>;
};

// --- 컴파일타임 검증용 Mock ---
namespace detail
{

struct MockProtocol
{
    struct Config
    {};
    struct Frame
    {};
    static Result<Frame> try_decode(RingBuffer&)
    {
        return Frame{};
    }
    static void consume_frame(RingBuffer&, const Frame&) {}
};

static_assert(Protocol<MockProtocol>, "MockProtocol must satisfy Protocol concept");

} // namespace detail

} // namespace apex::core
