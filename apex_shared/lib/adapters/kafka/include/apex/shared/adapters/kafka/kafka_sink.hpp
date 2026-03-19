#pragma once

#include <apex/shared/adapters/kafka/kafka_producer.hpp>

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <string>

namespace apex::shared::adapters::kafka
{

/// spdlog sink -> Kafka topic.
///
/// Inherits `spdlog::sinks::base_sink<spdlog::details::null_mutex>`:
/// - rd_kafka_produce() is thread-safe, so spdlog-side mutex is unnecessary
/// - Dedicated log topic to avoid bandwidth contention with business messages
///
/// Message format (JSON):
///   {"ts":"2026-03-13T12:00:00.000Z","level":"info","logger":"apex",
///    "msg":"...", "trace_id":"..."}
///
/// trace_id is extracted from spdlog MDC (future: core trace context).
///
/// Usage:
///   auto kafka_sink = std::make_shared<KafkaSink>(producer, "apex-logs");
///   auto logger = std::make_shared<spdlog::logger>("apex", kafka_sink);
class KafkaSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
{
  public:
    /// @param producer KafkaProducer reference (caller guarantees lifetime)
    /// @param topic Log-dedicated topic name
    KafkaSink(KafkaProducer& producer, std::string topic);

    ~KafkaSink() override = default;

  protected:
    /// Produce log message to Kafka.
    void sink_it_(const spdlog::details::log_msg& msg) override;

    /// Flush (Kafka produce is async, so call producer.poll())
    void flush_() override;

  private:
    /// Convert spdlog log_msg -> JSON string
    [[nodiscard]] std::string format_json(const spdlog::details::log_msg& msg) const;

    KafkaProducer& producer_;
    std::string topic_;
};

} // namespace apex::shared::adapters::kafka
