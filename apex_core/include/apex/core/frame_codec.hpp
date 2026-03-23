// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>

#include <cstdint>
#include <span>

namespace apex::core
{

/// Result of a successful frame extraction.
/// WARNING: payload_data points into RingBuffer's internal memory (zero-copy).
/// - payload_data is invalidated by consume_frame(), linearize(), or any RingBuffer mutation.
/// - Copy payload data before calling consume_frame() if you need it later.
struct Frame
{
    WireHeader header;
    std::span<const uint8_t> payload_data; // points into RingBuffer — valid until next RingBuffer mutation

    [[nodiscard]] std::span<const uint8_t> payload() const noexcept
    {
        return payload_data;
    }
};

/// Stateless frame codec. Extracts complete frames from a RingBuffer.
/// Uses RingBuffer::linearize() for zero-copy access when possible.
///
/// Usage:
///   RingBuffer buf(4096);
///   // ... recv data into buf ...
///   while (auto frame = FrameCodec::try_decode(buf)) {
///       process(frame->header, frame->payload());
///       FrameCodec::consume_frame(buf, *frame);
///   }
class FrameCodec
{
  public:
    [[nodiscard]] static Result<Frame> try_decode(RingBuffer& buf);

    static void consume_frame(RingBuffer& buf, const Frame& frame);

    [[nodiscard]] static bool encode(RingBuffer& buf, const WireHeader& header, std::span<const uint8_t> payload);

    [[nodiscard]] static size_t encode_to(std::span<uint8_t> out, const WireHeader& header,
                                          std::span<const uint8_t> payload);
};

} // namespace apex::core
