// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/message_router.hpp>

#include <apex/gateway/gateway_error.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace apex::gateway
{

using namespace apex::shared::protocols::kafka;

MessageRouter::MessageRouter(apex::shared::adapters::kafka::KafkaAdapter& kafka, RouteTablePtr initial_table,
                             uint16_t core_id, std::string response_topic)
    : kafka_(kafka)
    , route_table_(std::move(initial_table))
    , core_id_(core_id)
    , response_topic_(std::move(response_topic))
{}

apex::core::Result<void> MessageRouter::route(apex::core::SessionPtr session, const apex::core::WireHeader& header,
                                              std::span<const uint8_t> payload, uint64_t user_id, uint64_t corr_id)
{
    // 1. Resolve topic from route table
    auto table = route_table_.load(std::memory_order_acquire);
    auto topic = table->resolve(header.msg_id);
    if (!topic)
    {
        spdlog::warn("No route for msg_id: {}", header.msg_id);
        return apex::core::error(apex::core::ErrorCode::ServiceError);
    }

    // 2. Serialize Kafka Envelope (wire boundary: SessionId → uint64_t)
    auto envelope =
        build_envelope(header, payload, apex::core::to_underlying(session->id()), user_id, corr_id, core_id_);

    // 3. Kafka produce (session_id as key for partition distribution)
    auto session_key = std::to_string(apex::core::to_underlying(session->id()));
    return kafka_.produce(*topic, session_key, std::span<const uint8_t>(envelope));
}

void MessageRouter::update_table(RouteTablePtr new_table)
{
    route_table_.store(std::move(new_table), std::memory_order_release);
}

RouteTablePtr MessageRouter::current_table() const
{
    return route_table_.load(std::memory_order_acquire);
}

uint64_t MessageRouter::generate_corr_id() noexcept
{
    // per-core so no atomic needed
    // Upper 16 bits: core_id, lower 48 bits: monotonic counter
    return (static_cast<uint64_t>(core_id_) << 48) | (++corr_counter_);
}

std::vector<uint8_t> MessageRouter::build_envelope(const apex::core::WireHeader& header,
                                                   std::span<const uint8_t> payload, uint64_t session_id,
                                                   uint64_t user_id, uint64_t corr_id, uint16_t core_id) const
{

    // Routing Header
    RoutingHeader rh;
    rh.msg_id = header.msg_id;
    rh.flags = 0; // Request, unicast, uncompressed
    // HAS_REPLY_TOPIC은 build_full_envelope이 reply_topic 유무에 따라 자동 설정

    // Metadata Prefix
    MetadataPrefix meta;
    meta.core_id = core_id;
    meta.corr_id = corr_id;
    meta.source_id = source_ids::GATEWAY;
    meta.session_id = session_id;
    meta.user_id = user_id;
    meta.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    // [RoutingHeader 8B] [MetadataPrefix 40B] [ReplyTopic 2+NB] [Payload]
    return build_full_envelope(rh, meta, response_topic_, payload);
}

} // namespace apex::gateway
