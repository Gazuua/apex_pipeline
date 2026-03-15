#include <apex/gateway/response_dispatcher.hpp>

#include <apex/core/cross_core_call.hpp>
#include <spdlog/spdlog.h>

#include <cstring>

namespace apex::gateway {

using namespace apex::shared::protocols::kafka;

ResponseDispatcher::ResponseDispatcher(
    apex::core::CoreEngine& engine,
    std::vector<PendingRequestsMap*> pending_maps,
    std::vector<apex::core::SessionManager*> session_mgrs)
    : engine_(engine)
    , pending_maps_(std::move(pending_maps))
    , session_mgrs_(std::move(session_mgrs)) {}

apex::core::Result<void>
ResponseDispatcher::on_response(std::span<const uint8_t> payload) {
    // 1. Parse Routing Header
    if (payload.size() < ENVELOPE_HEADER_SIZE) {
        spdlog::warn("Response too short: {} bytes", payload.size());
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    auto rh_result = RoutingHeader::parse(payload.subspan(0, RoutingHeader::SIZE));
    if (!rh_result) {
        spdlog::warn("Invalid routing header in response");
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    // 2. Parse Metadata
    auto meta_result = MetadataPrefix::parse(
        payload.subspan(RoutingHeader::SIZE, MetadataPrefix::SIZE));
    if (!meta_result) {
        spdlog::warn("Invalid metadata in response");
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    auto& routing = *rh_result;
    auto& meta = *meta_result;

    // 3. Extract pending from target core's PendingMap
    uint16_t target_core = meta.core_id;
    if (target_core >= pending_maps_.size()) {
        spdlog::warn("Invalid core_id {} in response", target_core);
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    auto pending = pending_maps_[target_core]->extract(meta.corr_id);
    if (!pending) {
        // Already timed out or duplicate response
        spdlog::debug("No pending request for corr_id: {}", meta.corr_id);
        return apex::core::ok();  // Silently ignore
    }

    // 4. Extract FlatBuffers payload
    auto fbs_payload = payload.subspan(ENVELOPE_HEADER_SIZE);

    // 5. Build WireHeader response
    auto wire_response = build_wire_response(
        routing, fbs_payload, pending->original_msg_id);

    // 6. Deliver response to target core's session
    auto session_id = pending->session_id;
    auto* session_mgr = session_mgrs_[target_core];

    // cross_core_post: fire-and-forget delivery on target core
    return apex::core::cross_core_post(
        engine_, target_core,
        [session_mgr, session_id,
         response = std::move(wire_response)]() {
            auto session = session_mgr->find_session(session_id);
            if (!session || !session->is_open()) return;
            (void)session->enqueue_write(
                std::vector<uint8_t>(response.begin(), response.end()));
        });
}

std::vector<uint8_t>
ResponseDispatcher::build_wire_response(
    const RoutingHeader& routing,
    std::span<const uint8_t> fbs_payload,
    uint32_t original_msg_id) {

    apex::core::WireHeader wh;
    wh.msg_id = original_msg_id;
    wh.body_size = static_cast<uint32_t>(fbs_payload.size());
    wh.flags = 0;

    // Reflect error bit from Routing Header to WireHeader flags
    if (routing.flags & routing_flags::ERROR_BIT) {
        wh.flags |= apex::core::wire_flags::ERROR_RESPONSE;
    }

    std::vector<uint8_t> buf(apex::core::WireHeader::SIZE + fbs_payload.size());
    auto hdr_buf = wh.serialize();
    std::memcpy(buf.data(), hdr_buf.data(), hdr_buf.size());

    if (!fbs_payload.empty()) {
        std::memcpy(buf.data() + apex::core::WireHeader::SIZE,
                    fbs_payload.data(), fbs_payload.size());
    }

    return buf;
}

} // namespace apex::gateway
