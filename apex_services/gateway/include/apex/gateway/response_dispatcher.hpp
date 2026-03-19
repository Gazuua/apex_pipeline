#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/result.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/gateway/pending_requests.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace apex::gateway
{

/// Kafka response reception -> original session WireHeader response delivery.
/// Called from Kafka consumer callback.
///
/// Processing flow:
/// 1. Decode Kafka Envelope (RoutingHeader + MetadataPrefix)
/// 2. MetadataPrefix.core_id -> target core identification
/// 3. MetadataPrefix.corr_id -> PendingRequestsMap lookup
/// 4. cross_core_post to deliver response on original core's session
class ResponseDispatcher
{
  public:
    ResponseDispatcher(apex::core::CoreEngine& engine, std::vector<PendingRequestsMap*> pending_maps,
                       std::vector<apex::core::SessionManager*> session_mgrs);

    /// Called from Kafka consumer callback.
    /// @param payload Full Kafka message (Envelope)
    [[nodiscard]] apex::core::Result<void> on_response(std::span<const uint8_t> payload);

  private:
    /// Envelope -> WireHeader conversion.
    [[nodiscard]] std::vector<uint8_t> build_wire_response(const apex::shared::protocols::kafka::RoutingHeader& routing,
                                                           std::span<const uint8_t> fbs_payload,
                                                           uint32_t original_msg_id);

    apex::core::CoreEngine& engine_;
    std::vector<PendingRequestsMap*> pending_maps_;
    std::vector<apex::core::SessionManager*> session_mgrs_;
};

} // namespace apex::gateway
