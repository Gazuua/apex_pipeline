#pragma once

#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/jwt_manager.hpp>
#include <apex/auth_svc/password_hasher.hpp>
#include <apex/auth_svc/session_store.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace apex::auth_svc {

/// Auth Service -- Kafka-based authentication request/response handler.
///
/// Consumes from `auth.requests` topic, processes login/logout/refresh,
/// and produces responses to `auth.responses` topic.
///
/// Does NOT inherit ServiceBase -- Kafka message-driven, not HTTP session-driven.
class AuthService {
public:
    explicit AuthService(
        AuthConfig config,
        apex::shared::adapters::kafka::KafkaAdapter& kafka,
        apex::shared::adapters::redis::RedisAdapter& redis,
        apex::shared::adapters::pg::PgAdapter& pg);

    ~AuthService();

    /// Start service: register Kafka consumer callback, init components
    void start();

    /// Stop service
    void stop();

    [[nodiscard]] std::string_view name() const noexcept { return "auth"; }

private:
    /// Kafka message receive callback
    apex::core::Result<void> on_kafka_message(
        std::string_view topic,
        int32_t partition,
        std::span<const uint8_t> key,
        std::span<const uint8_t> payload,
        int64_t offset);

    /// Parse Kafka Envelope (RoutingHeader + Metadata) and dispatch by msg_id
    void dispatch_envelope(std::span<const uint8_t> payload);

    /// Login request handler
    void handle_login(std::span<const uint8_t> fbs_payload,
                      uint64_t corr_id,
                      uint16_t core_id,
                      uint64_t session_id);

    /// Logout request handler
    void handle_logout(std::span<const uint8_t> fbs_payload,
                       uint64_t corr_id,
                       uint16_t core_id,
                       uint64_t session_id);

    /// Refresh Token request handler
    void handle_refresh_token(std::span<const uint8_t> fbs_payload,
                              uint64_t corr_id,
                              uint16_t core_id,
                              uint64_t session_id);

    /// Build response Kafka Envelope and produce
    void send_response(uint32_t msg_id,
                       uint64_t corr_id,
                       uint16_t core_id,
                       uint64_t session_id,
                       std::span<const uint8_t> fbs_payload);

    AuthConfig config_;
    apex::shared::adapters::kafka::KafkaAdapter& kafka_;
    apex::shared::adapters::redis::RedisAdapter& redis_;
    apex::shared::adapters::pg::PgAdapter& pg_;

    JwtManager jwt_manager_;
    PasswordHasher password_hasher_;
    SessionStore session_store_;

    bool started_{false};
};

} // namespace apex::auth_svc
