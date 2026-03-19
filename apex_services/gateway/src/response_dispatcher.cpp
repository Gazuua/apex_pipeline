#include <apex/gateway/response_dispatcher.hpp>

#include <apex/core/cross_core_call.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <cstring>

namespace apex::gateway
{

using namespace apex::shared::protocols::kafka;

ResponseDispatcher::ResponseDispatcher(apex::core::CoreEngine& engine, std::vector<PendingRequestsMap*> pending_maps,
                                       std::vector<apex::core::SessionManager*> session_mgrs)
    : engine_(engine)
    , pending_maps_(std::move(pending_maps))
    , session_mgrs_(std::move(session_mgrs))
{}

apex::core::Result<void> ResponseDispatcher::on_response(std::span<const uint8_t> payload)
{
    // 1. Parse Routing Header (stateless — safe on any thread)
    if (payload.size() < ENVELOPE_HEADER_SIZE)
    {
        spdlog::warn("Response too short: {} bytes", payload.size());
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    auto rh_result = RoutingHeader::parse(payload.subspan(0, RoutingHeader::SIZE));
    if (!rh_result)
    {
        spdlog::warn("Invalid routing header in response");
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    // 2. Parse Metadata (stateless — safe on any thread)
    auto meta_result = MetadataPrefix::parse(payload.subspan(RoutingHeader::SIZE, MetadataPrefix::SIZE));
    if (!meta_result)
    {
        spdlog::warn("Invalid metadata in response");
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    auto& routing = *rh_result;
    auto& meta = *meta_result;

    // 3. Validate core_id range
    uint16_t target_core = meta.core_id;
    if (target_core >= pending_maps_.size())
    {
        spdlog::warn("Invalid core_id {} in response", target_core);
        return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    // 4. Copy payload for capture (Kafka buffer lifetime is callback-scoped)
    auto payload_copy = std::make_shared<std::vector<uint8_t>>(payload.begin(), payload.end());

    // 5. Post to target core's io_context so PendingRequestsMap access
    //    and session write happen on the core thread — no data race.
    auto corr_id = meta.corr_id;
    auto routing_copy = routing;

    boost::asio::post(engine_.io_context(target_core), [this, target_core, corr_id, routing_copy,
                                                        payload_copy = std::move(payload_copy)]() {
        // Now on target core's thread — safe to access PendingRequestsMap
        auto pending = pending_maps_[target_core]->extract(corr_id);
        if (!pending)
        {
            spdlog::debug("No pending request for corr_id: {}", corr_id);
            return;
        }

        // Build WireHeader response (skip envelope headers + reply topic)
        auto payload_offset = envelope_payload_offset(routing_copy.flags, std::span<const uint8_t>(*payload_copy));
        auto fbs_payload =
            std::span<const uint8_t>(payload_copy->data() + payload_offset, payload_copy->size() - payload_offset);

        // Use response msg_id from RoutingHeader (set by service),
        // not the original request msg_id from PendingRequest.
        auto wire_response = build_wire_response(routing_copy, fbs_payload, routing_copy.msg_id);

        // Deliver to session (already on the correct core thread)
        auto* session_mgr = session_mgrs_[target_core];
        auto session = session_mgr->find_session(pending->session_id);
        if (!session || !session->is_open())
            return;
        (void)session->enqueue_write(std::move(wire_response));
    });

    return apex::core::ok();
}

std::vector<uint8_t> ResponseDispatcher::build_wire_response(const RoutingHeader& routing,
                                                             std::span<const uint8_t> fbs_payload,
                                                             uint32_t original_msg_id)
{

    apex::core::WireHeader wh;
    wh.msg_id = original_msg_id;
    wh.body_size = static_cast<uint32_t>(fbs_payload.size());
    wh.flags = 0;

    // Reflect error bit from Routing Header to WireHeader flags
    if (routing.flags & routing_flags::ERROR_BIT)
    {
        wh.flags |= apex::core::wire_flags::ERROR_RESPONSE;
    }

    std::vector<uint8_t> buf(apex::core::WireHeader::SIZE + fbs_payload.size());
    auto hdr_buf = wh.serialize();
    std::memcpy(buf.data(), hdr_buf.data(), hdr_buf.size());

    if (!fbs_payload.empty())
    {
        std::memcpy(buf.data() + apex::core::WireHeader::SIZE, fbs_payload.data(), fbs_payload.size());
    }

    return buf;
}

} // namespace apex::gateway
