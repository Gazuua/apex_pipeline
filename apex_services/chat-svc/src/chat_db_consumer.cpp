// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "chat_db_consumer.hpp"

#include <chat_message_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <string>

namespace apex::chat_svc
{

ChatDbConsumer::ChatDbConsumer(apex::shared::adapters::pg::PgAdapter& pg)
    : pg_(pg)
{}

apex::core::Result<void> ChatDbConsumer::on_message(std::string_view /*topic*/, int32_t /*partition*/,
                                                    std::span<const uint8_t> /*key*/, std::span<const uint8_t> payload,
                                                    int64_t /*offset*/)
{
    // 1. FlatBuffers verification
    flatbuffers::Verifier verifier(payload.data(), payload.size());
    if (!verifier.VerifyBuffer<fbs::ChatMessage>())
    {
        spdlog::error("[ChatDbConsumer] ChatMessage verification failed");
        return apex::core::error(apex::core::ErrorCode::FlatBuffersVerifyFailed);
    }

    auto* msg = flatbuffers::GetRoot<fbs::ChatMessage>(payload.data());

    auto room_id = msg->room_id();
    auto sender_id = msg->sender_id();
    auto sender_name = msg->sender_name() ? msg->sender_name()->string_view() : std::string_view{};
    auto content = msg->content() ? msg->content()->string_view() : std::string_view{};
    auto timestamp = msg->timestamp();

    // 2. PostgreSQL INSERT
    // NOTE: Kafka consumer callback is synchronous. In production,
    // this would use co_spawn into a core's io_context or a synchronous
    // libpq call. Current implementation logs and returns success as
    // a placeholder -- full async integration in E2E phase (Plan 5).
    spdlog::info("[ChatDbConsumer] Persisting message: room={}, sender={}, ts={}", room_id, sender_id, timestamp);

    // The actual INSERT would be:
    // INSERT INTO chat_svc.chat_messages
    //   (room_id, sender_id, sender_name, content, message_type, created_at)
    // VALUES ($1, $2, $3, $4, $5, to_timestamp($6::bigint / 1000.0))
    //
    // params: room_id, sender_id, sender_name, content, 0 (normal), timestamp

    (void)room_id;
    (void)sender_id;
    (void)sender_name;
    (void)content;
    (void)timestamp;

    return apex::core::ok();
}

} // namespace apex::chat_svc
