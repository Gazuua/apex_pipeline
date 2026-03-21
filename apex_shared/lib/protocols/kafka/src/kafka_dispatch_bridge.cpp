// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp>

namespace apex::shared::protocols::kafka
{

KafkaDispatchBridge::KafkaDispatchBridge(const HandlerMap& handlers)
    : handlers_(&handlers)
{}

boost::asio::awaitable<apex::core::Result<void>> KafkaDispatchBridge::dispatch(std::span<const uint8_t> raw_message)
{
    // 최소 크기 검증: RoutingHeader(8) + MetadataPrefix(40) = 48바이트
    if (raw_message.size() < ENVELOPE_HEADER_SIZE)
    {
        co_return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }

    if (!handlers_)
    {
        co_return apex::core::error(apex::core::ErrorCode::HandlerNotFound);
    }

    // RoutingHeader 파싱
    auto routing_result = RoutingHeader::parse(raw_message);
    if (!routing_result.has_value())
    {
        co_return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }
    const auto& routing = routing_result.value();

    // MetadataPrefix 파싱 (RoutingHeader 이후 오프셋)
    auto meta_result = MetadataPrefix::parse(raw_message.subspan(RoutingHeader::SIZE));
    if (!meta_result.has_value())
    {
        co_return apex::core::error(apex::core::ErrorCode::ParseFailed);
    }
    const auto& metadata = meta_result.value();

    // Payload 오프셋 계산 (reply_topic 유무에 따라 가변)
    const size_t payload_offset = envelope_payload_offset(routing.flags, raw_message);

    // Payload 추출
    std::span<const uint8_t> payload;
    if (payload_offset < raw_message.size())
    {
        payload = raw_message.subspan(payload_offset);
    }

    // 핸들러 조회
    auto it = handlers_->find(routing.msg_id);
    if (it == handlers_->end())
    {
        co_return apex::core::error(apex::core::ErrorCode::HandlerNotFound);
    }

    // MetadataPrefix → KafkaMessageMeta 변환 (session_id: uint64_t → SessionId 강타입)
    apex::core::KafkaMessageMeta meta{
        .meta_version = metadata.meta_version,
        .core_id = metadata.core_id,
        .corr_id = metadata.corr_id,
        .source_id = metadata.source_id,
        .session_id = apex::core::make_session_id(metadata.session_id),
        .user_id = metadata.user_id,
        .timestamp = metadata.timestamp,
    };

    // 핸들러 디스패치
    co_return co_await it->second(meta, routing.msg_id, payload);
}

bool KafkaDispatchBridge::has_handler(uint32_t msg_id) const noexcept
{
    return handlers_ && handlers_->contains(msg_id);
}

} // namespace apex::shared::protocols::kafka
