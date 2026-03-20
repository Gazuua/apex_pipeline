// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/frame_codec.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace apex::core
{

std::expected<Frame, FrameError> FrameCodec::try_decode(RingBuffer& buf)
{
    if (buf.readable_size() < WireHeader::SIZE)
    {
        return std::unexpected(FrameError::InsufficientData);
    }

    // I-08: Defensive copy — even when the header is already contiguous in the ring buffer,
    // we copy to a local array. This ensures safety against subsequent linearize() calls
    // that may invalidate the span. Benchmarking can determine if an optimization for the
    // contiguous path is worthwhile; 12 bytes is unlikely to be a measurable overhead.
    std::array<uint8_t, WireHeader::SIZE> hdr_buf;
    auto hdr_span = buf.linearize(WireHeader::SIZE);
    std::memcpy(hdr_buf.data(), hdr_span.data(), WireHeader::SIZE);

    auto parse_result = WireHeader::parse(hdr_buf);

    if (!parse_result)
    {
        auto err = parse_result.error();
        if (err == ParseError::BodyTooLarge)
        {
            return std::unexpected(FrameError::BodyTooLarge);
        }
        // I-19: ParseError::UnsupportedVersion is mapped to FrameError::HeaderParseError.
        // TODO: Add FrameError::UnsupportedProtocolVersion for finer-grained error handling
        // once frame_codec.hpp is updated. ErrorCode::UnsupportedProtocolVersion is available.
        return std::unexpected(FrameError::HeaderParseError);
    }

    auto header = *parse_result;
    size_t total = header.frame_size();

    if (buf.readable_size() < total)
    {
        return std::unexpected(FrameError::InsufficientData);
    }

    // NOTE: 이 linearize()는 hdr_span을 무효화할 수 있지만,
    // 이미 hdr_buf에 복사했으므로 안전하다.
    auto frame_span = buf.linearize(total);
    auto payload = frame_span.subspan(WireHeader::SIZE, header.body_size);

    return Frame{header, payload};
}

void FrameCodec::consume_frame(RingBuffer& buf, const Frame& frame)
{
    buf.consume(frame.header.frame_size());
}

bool FrameCodec::encode(RingBuffer& buf, const WireHeader& header, std::span<const uint8_t> payload)
{
    if (header.body_size != static_cast<uint32_t>(payload.size()))
    {
        return false;
    }

    size_t total = WireHeader::SIZE + payload.size();
    if (buf.writable_size() < total)
    {
        return false;
    }

    auto header_bytes = header.serialize();

    // Write header - may need multiple writable() calls due to wrap-around
    size_t header_written = 0;
    while (header_written < WireHeader::SIZE)
    {
        auto w = buf.writable();
        if (w.empty())
            return false; // defensive guard against infinite loop
        size_t to_write = std::min(w.size(), WireHeader::SIZE - header_written);
        std::memcpy(w.data(), header_bytes.data() + header_written, to_write);
        buf.commit_write(to_write);
        header_written += to_write;
    }

    // Write payload - may need multiple writable() calls due to wrap-around
    size_t payload_written = 0;
    while (payload_written < payload.size())
    {
        auto w = buf.writable();
        if (w.empty())
            return false; // defensive guard against infinite loop
        size_t to_write = std::min(w.size(), payload.size() - payload_written);
        std::memcpy(w.data(), payload.data() + payload_written, to_write);
        buf.commit_write(to_write);
        payload_written += to_write;
    }

    return true;
}

size_t FrameCodec::encode_to(std::span<uint8_t> out, const WireHeader& header, std::span<const uint8_t> payload)
{
    if (header.body_size != static_cast<uint32_t>(payload.size()))
    {
        return 0;
    }

    size_t total = WireHeader::SIZE + payload.size();
    if (out.size() < total)
    {
        return 0;
    }

    auto header_bytes = header.serialize();
    std::memcpy(out.data(), header_bytes.data(), WireHeader::SIZE);
    if (!payload.empty())
    {
        std::memcpy(out.data() + WireHeader::SIZE, payload.data(), payload.size());
    }

    return total;
}

} // namespace apex::core
