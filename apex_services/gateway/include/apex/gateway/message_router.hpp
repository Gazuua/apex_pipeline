#pragma once

#include <apex/gateway/route_table.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/core/result.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace apex::gateway {

/// msg_id -> Kafka topic routing + Envelope conversion.
/// RouteTablePtr is atomically swappable for hot-reload support.
class MessageRouter {
public:
    MessageRouter(apex::shared::adapters::kafka::KafkaAdapter& kafka,
                  RouteTablePtr initial_table,
                  uint32_t core_id);

    /// WireHeader -> Kafka Envelope conversion + produce.
    /// @param session Request session (for session_id)
    /// @param header Client WireHeader
    /// @param payload Message body
    /// @param user_id User ID from JWT
    /// @param corr_id Unique correlation ID (Gateway-generated)
    [[nodiscard]] apex::core::Result<void>
    route(apex::core::SessionPtr session,
          const apex::core::WireHeader& header,
          std::span<const uint8_t> payload,
          uint64_t user_id,
          uint64_t corr_id);

    /// Atomic route table replacement (hot-reload).
    void update_table(RouteTablePtr new_table);

    /// Current route table.
    [[nodiscard]] RouteTablePtr current_table() const;

    /// Generate correlation ID (per-core monotonic counter).
    [[nodiscard]] uint64_t generate_corr_id() noexcept;

private:
    /// Kafka Envelope serialization.
    [[nodiscard]] std::vector<uint8_t>
    build_envelope(const apex::core::WireHeader& header,
                   std::span<const uint8_t> payload,
                   uint64_t session_id,
                   uint64_t corr_id,
                   uint16_t core_id);

    apex::shared::adapters::kafka::KafkaAdapter& kafka_;
    std::atomic<std::shared_ptr<const RouteTable>> route_table_;
    uint32_t core_id_;
    uint64_t corr_counter_{0};  // per-core, lock-free
};

} // namespace apex::gateway
