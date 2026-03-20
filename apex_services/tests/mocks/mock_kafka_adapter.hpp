// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

/// Mock KafkaAdapter — produce/consume 기록, 큐 기반 테스트용.
/// 실제 Kafka 연결 없이 서비스 핸들러의 Kafka 상호작용을 검증.

#include <apex/core/result.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::test
{

/// Produced message record.
struct ProducedMessage
{
    std::string topic;
    std::string key;
    std::vector<uint8_t> payload;
};

/// Kafka consumer message callback (same signature as real KafkaConsumer).
using MessageCallback =
    std::function<apex::core::Result<void>(std::string_view topic, int32_t partition, std::span<const uint8_t> key,
                                           std::span<const uint8_t> payload, int64_t offset)>;

/// Mock KafkaAdapter.
/// Thread-safe: uses mutex for produce/consume records.
/// Provides the same API surface as KafkaAdapter that services actually use.
class MockKafkaAdapter
{
  public:
    MockKafkaAdapter() = default;

    // --- produce API (mirrors KafkaAdapter::produce) ---

    [[nodiscard]] apex::core::Result<void> produce(std::string_view topic, std::string_view key,
                                                   std::span<const uint8_t> payload)
    {
        if (fail_produce_)
        {
            return apex::core::error(apex::core::ErrorCode::AdapterError);
        }
        std::lock_guard lock(mu_);
        produced_.push_back(ProducedMessage{
            .topic = std::string(topic),
            .key = std::string(key),
            .payload = std::vector<uint8_t>(payload.begin(), payload.end()),
        });
        return apex::core::ok();
    }

    [[nodiscard]] apex::core::Result<void> produce(std::string_view topic, std::string_view key,
                                                   std::string_view payload)
    {
        auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        return produce(topic, key, bytes);
    }

    // --- consumer API ---

    void set_message_callback(MessageCallback cb)
    {
        message_cb_ = std::move(cb);
    }

    /// Inject a message as if consumed from Kafka.
    apex::core::Result<void> inject_message(std::string_view topic, int32_t partition, std::span<const uint8_t> key,
                                            std::span<const uint8_t> payload, int64_t offset = 0)
    {
        if (message_cb_)
        {
            return message_cb_(topic, partition, key, payload, offset);
        }
        return apex::core::ok();
    }

    // --- Test inspection ---

    [[nodiscard]] const std::vector<ProducedMessage>& produced() const
    {
        return produced_;
    }

    [[nodiscard]] size_t produce_count() const
    {
        std::lock_guard lock(mu_);
        return produced_.size();
    }

    void clear()
    {
        std::lock_guard lock(mu_);
        produced_.clear();
    }

    /// Set produce to fail with AdapterError.
    void set_fail_produce(bool fail)
    {
        fail_produce_ = fail;
    }

  private:
    mutable std::mutex mu_;
    std::vector<ProducedMessage> produced_;
    MessageCallback message_cb_;
    bool fail_produce_{false};
};

} // namespace apex::test
