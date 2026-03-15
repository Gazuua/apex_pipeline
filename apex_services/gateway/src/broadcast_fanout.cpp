#include <apex/gateway/broadcast_fanout.hpp>

#include <apex/core/cross_core_call.hpp>
#include <spdlog/spdlog.h>

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

    // Service publishes complete WireHeader + payload frame to Redis.
    // Gateway forwards it to sessions as-is (opaque).
    auto data = std::make_shared<std::vector<uint8_t>>(
        message.begin(), message.end());

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

} // namespace apex::gateway
