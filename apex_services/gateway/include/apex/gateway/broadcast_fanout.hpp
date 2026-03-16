#pragma once

#include <apex/gateway/channel_session_map.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/wire_header.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace apex::gateway {

/// Redis Pub/Sub message prefix: 4-byte big-endian msg_id before the payload.
/// Services MUST publish messages in this format:
///   [4B msg_id (BE)] + [FlatBuffers payload]
/// Gateway wraps this into WireHeader v2 frame before sending to clients.
static constexpr size_t PUBSUB_MSG_ID_SIZE = 4;

/// PubSub message -> subscriber session fan-out.
/// Called from PubSubListener's callback.
/// Posts to each core via cross_core_post to deliver to that core's sessions.
///
/// Redis message format:  [4B msg_id BE] + [payload]
/// Client wire format:    [12B WireHeader v2] + [payload]
class BroadcastFanout {
public:
    BroadcastFanout(apex::core::CoreEngine& engine,
                    const ChannelSessionMap& channel_map,
                    std::vector<apex::core::SessionManager*> session_mgrs);

    /// Fan-out channel message to subscriber sessions.
    /// @param channel Channel name
    /// @param message Redis Pub/Sub message ([4B msg_id BE] + [payload])
    void fanout(std::string_view channel,
                std::span<const uint8_t> message);

private:
    /// Build WireHeader v2 framed data from Redis Pub/Sub message.
    /// @param message [4B msg_id BE] + [payload]
    /// @return [12B WireHeader v2] + [payload], or empty on error
    [[nodiscard]] static std::vector<uint8_t>
    build_wire_frame(std::span<const uint8_t> message);

    apex::core::CoreEngine& engine_;
    const ChannelSessionMap& channel_map_;
    std::vector<apex::core::SessionManager*> session_mgrs_;
};

} // namespace apex::gateway
