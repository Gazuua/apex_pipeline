// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/gateway/channel_session_map.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace apex::gateway
{

/// Redis Pub/Sub message prefix: 4-byte big-endian msg_id before the payload.
/// Services MUST publish messages in this format:
///   [4B msg_id (BE)] + [FlatBuffers payload]
/// Gateway wraps this into WireHeader v2 frame before sending to clients.
static constexpr size_t PUBSUB_MSG_ID_SIZE = 4;

/// PubSub message -> subscriber session fan-out.
/// Called from PubSubListener's callback.
/// Posts to ALL cores via cross_core_post; each core checks its local
/// per-core ChannelSessionMap for subscribers.
///
/// Redis message format:  [4B msg_id BE] + [payload]
/// Client wire format:    [12B WireHeader v2] + [payload]
class BroadcastFanout
{
  public:
    BroadcastFanout(apex::core::CoreEngine& engine, uint32_t num_cores,
                    std::vector<apex::core::SessionManager*> session_mgrs);

    /// per-core 채널 맵 바인딩. create_globals() → server.global<T>() move 후
    /// on_wire()에서 globals_ 포인터 확정 시점에 호출한다.
    void set_channel_maps(std::vector<ChannelSessionMap>* maps);

    /// Fan-out channel message to subscriber sessions.
    /// 모든 코어에 post → 각 코어가 로컬 맵에서 구독자 확인 후 전송.
    /// @param channel Channel name
    /// @param message Redis Pub/Sub message ([4B msg_id BE] + [payload])
    void fanout(std::string_view channel, std::span<const uint8_t> message);

    /// Build WireHeader v2 framed data from Redis Pub/Sub message.
    /// @param message [4B msg_id BE] + [payload]
    /// @return [12B WireHeader v2] + [payload], or empty on error
    [[nodiscard]] static std::vector<uint8_t> build_wire_frame(std::span<const uint8_t> message);

  private:
    apex::core::CoreEngine& engine_;
    uint32_t num_cores_;
    std::vector<apex::core::SessionManager*> session_mgrs_;
    std::vector<ChannelSessionMap>* channel_maps_{nullptr};
    apex::core::ScopedLogger logger_{"BroadcastFanout", apex::core::ScopedLogger::NO_CORE, "app"};
};

} // namespace apex::gateway
