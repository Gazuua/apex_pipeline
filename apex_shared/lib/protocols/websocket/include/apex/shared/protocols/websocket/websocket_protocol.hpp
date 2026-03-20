// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/protocol.hpp>
#include <apex/core/ring_buffer.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace apex::shared::protocols::websocket
{

/// WebSocket 프로토콜 — Protocol concept 구현 (MVP/스켈레톤).
///
/// 설계 방향:
///   - Boost.Beast의 websocket::stream이 framing을 담당
///   - try_decode()는 RingBuffer에서 이미 수신된 WebSocket 메시지를 추출
///   - 실제 Beast 통합은 v0.5.1+에서 완성 (handshake, binary/text 분리 등)
///
/// 현재 구현:
///   - Protocol concept을 만족하는 최소 스켈레톤
///   - try_decode()는 길이-접두어(4바이트 LE) + 페이로드 방식으로 메시지 추출
///   - Beast 완전 통합 시 이 구현을 대체할 예정
struct WebSocketProtocol
{
    struct Config
    {
        std::string path = "/ws";
        size_t max_message_size = 1 * 1024 * 1024; // 1MB
    };

    struct Frame
    {
        std::vector<uint8_t> payload_data;
        bool is_text = false;
        bool is_binary = true;

        [[nodiscard]] std::span<const uint8_t> payload() const noexcept
        {
            return {payload_data.data(), payload_data.size()};
        }
    };

    /// RingBuffer에서 WebSocket 메시지 추출 (MVP: 길이-접두어 방식).
    /// Beast 완전 통합 시 Beast 내부 버퍼 기반으로 교체 예정.
    [[nodiscard]] static apex::core::Result<Frame> try_decode(apex::core::RingBuffer& buf);

    /// 디코딩된 프레임을 RingBuffer에서 소비.
    static void consume_frame(apex::core::RingBuffer& buf, const Frame& frame);
};

// Concept 검증
static_assert(apex::core::Protocol<WebSocketProtocol>);

} // namespace apex::shared::protocols::websocket
