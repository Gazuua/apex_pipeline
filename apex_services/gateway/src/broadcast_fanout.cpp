#include <apex/gateway/broadcast_fanout.hpp>

#include <apex/core/cross_core_call.hpp>
#include <spdlog/spdlog.h>

#include <cstring>

namespace apex::gateway {

BroadcastFanout::BroadcastFanout(
    apex::core::CoreEngine& engine,
    const ChannelSessionMap& channel_map,
    std::vector<apex::core::SessionManager*> session_mgrs)
    : engine_(engine)
    , channel_map_(channel_map)
    , session_mgrs_(std::move(session_mgrs)) {}

void BroadcastFanout::fanout(std::string_view channel,
                             std::span<const uint8_t> message) {
    auto groups = channel_map_.get_subscribers(std::string(channel));
    if (groups.empty()) return;

    // Build WireHeader v2 framed data from Redis Pub/Sub message
    auto frame = build_wire_frame(message);
    if (frame.empty()) {
        spdlog::warn("BroadcastFanout: failed to build wire frame for channel '{}'",
                     channel);
        return;
    }

    auto data = std::make_shared<std::vector<uint8_t>>(std::move(frame));

    for (const auto& group : groups) {
        if (group.core_id >= session_mgrs_.size()) {
            spdlog::warn("BroadcastFanout: invalid core_id {}", group.core_id);
            continue;
        }

        auto session_ids = group.session_ids;
        auto core_id = group.core_id;
        auto* mgr = session_mgrs_[core_id];

        (void)apex::core::cross_core_post(
            engine_, core_id,
            [mgr, session_ids = std::move(session_ids),
             shared_data = data]() {
                for (auto sid : session_ids) {
                    auto session = mgr->find_session(sid);
                    if (!session || !session->is_open()) continue;
                    (void)session->enqueue_write_raw(*shared_data);
                }
            });
    }
}

std::vector<uint8_t>
BroadcastFanout::build_wire_frame(std::span<const uint8_t> message) {
    // Redis Pub/Sub message format: [4B msg_id BE] + [payload]
    // If message is shorter than 4 bytes, treat as system broadcast (msg_id=0, no payload).
    uint32_t msg_id = 0;
    std::span<const uint8_t> payload;

    if (message.size() >= PUBSUB_MSG_ID_SIZE) {
        // Extract big-endian msg_id
        msg_id = (static_cast<uint32_t>(message[0]) << 24) |
                 (static_cast<uint32_t>(message[1]) << 16) |
                 (static_cast<uint32_t>(message[2]) << 8)  |
                 (static_cast<uint32_t>(message[3]));
        payload = message.subspan(PUBSUB_MSG_ID_SIZE);
    } else if (!message.empty()) {
        // Malformed — too short for msg_id prefix
        spdlog::warn("BroadcastFanout: message too short ({} bytes) for msg_id prefix",
                     message.size());
        return {};
    }
    // else: empty message -> msg_id=0, empty payload (e.g., heartbeat/signal)

    // Build WireHeader v2
    apex::core::WireHeader wh;
    wh.msg_id = msg_id;
    wh.body_size = static_cast<uint32_t>(payload.size());
    wh.flags = 0;

    // Assemble [12B WireHeader] + [payload]
    std::vector<uint8_t> frame(apex::core::WireHeader::SIZE + payload.size());
    auto hdr_buf = wh.serialize();
    std::memcpy(frame.data(), hdr_buf.data(), hdr_buf.size());

    if (!payload.empty()) {
        std::memcpy(frame.data() + apex::core::WireHeader::SIZE,
                    payload.data(), payload.size());
    }

    return frame;
}

} // namespace apex::gateway
