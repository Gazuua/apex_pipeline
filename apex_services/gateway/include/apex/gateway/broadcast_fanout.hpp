#pragma once

#include <apex/gateway/channel_session_map.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/session_manager.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace apex::gateway {

/// PubSub message -> subscriber session fan-out.
/// Called from PubSubListener's callback.
/// Posts to each core via cross_core_post to deliver to that core's sessions.
class BroadcastFanout {
public:
    BroadcastFanout(apex::core::CoreEngine& engine,
                    const ChannelSessionMap& channel_map,
                    std::vector<apex::core::SessionManager*> session_mgrs);

    /// Fan-out channel message to subscriber sessions.
    /// @param channel Channel name
    /// @param message Message data (complete WireHeader + payload frame from service)
    void fanout(std::string_view channel,
                std::span<const uint8_t> message);

private:
    apex::core::CoreEngine& engine_;
    const ChannelSessionMap& channel_map_;
    std::vector<apex::core::SessionManager*> session_mgrs_;
};

} // namespace apex::gateway
