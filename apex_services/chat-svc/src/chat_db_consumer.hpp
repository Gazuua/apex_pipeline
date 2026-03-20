// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace apex::chat_svc
{

/// Kafka -> PostgreSQL history persistence consumer.
/// Runs as a separate consumer group ("chat-db-writer") consuming
/// the chat.messages.persist topic and INSERTing into chat_svc.chat_messages.
///
/// On failure, messages are routed to DLQ (chat.messages.persist.dlq).
class ChatDbConsumer
{
  public:
    explicit ChatDbConsumer(apex::shared::adapters::pg::PgAdapter& pg);

    /// KafkaConsumer MessageCallback registration target.
    apex::core::Result<void> on_message(std::string_view topic, int32_t partition, std::span<const uint8_t> key,
                                        std::span<const uint8_t> payload, int64_t offset);

  private:
    apex::shared::adapters::pg::PgAdapter& pg_;
};

} // namespace apex::chat_svc
