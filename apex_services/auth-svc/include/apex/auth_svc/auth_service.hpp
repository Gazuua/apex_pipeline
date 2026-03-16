#pragma once

#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/jwt_manager.hpp>
#include <apex/auth_svc/password_hasher.hpp>
#include <apex/auth_svc/session_store.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace apex::auth_svc {

/// Cached Kafka envelope metadata for the currently dispatching message.
/// Set in dispatch_envelope() before dispatcher_.dispatch(), read by handlers.
/// Thread-safe: Kafka consumer callback is single-threaded sequential.
struct EnvelopeMetadata {
    uint64_t corr_id = 0;
    uint16_t core_id = 0;
    uint64_t session_id = 0;
    std::string reply_topic;
};

/// Auth Service -- Kafka-based authentication request/response handler.
///
/// Consumes from `auth.requests` topic, processes login/logout/refresh,
/// and produces responses to `auth.responses` topic.
///
/// Does NOT inherit ServiceBase -- Kafka message-driven, not HTTP session-driven.
/// Uses MessageDispatcher (O(1) hash map) for msg_id-based dispatch.
/// Handlers are awaitable coroutines; Kafka callback bridges via co_spawn.
class AuthService {
public:
    explicit AuthService(
        AuthConfig config,
        boost::asio::any_io_executor executor,
        apex::shared::adapters::kafka::KafkaAdapter& kafka,
        apex::shared::adapters::redis::RedisAdapter& redis,
        apex::shared::adapters::pg::PgAdapter& pg);

    ~AuthService();

    /// Start service: register handlers to MessageDispatcher, set Kafka callback
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

    /// Parse Kafka Envelope (RoutingHeader + Metadata) and dispatch by msg_id.
    /// Synchronous — called from Kafka consumer thread.
    /// Internally co_spawns a coroutine on executor_ for async handler execution.
    void dispatch_envelope(std::span<const uint8_t> payload);

    /// Login request handler (awaitable coroutine).
    /// Metadata accessed via current_meta_ (set before dispatch).
    boost::asio::awaitable<apex::core::Result<void>> handle_login(
        apex::core::SessionPtr session,
        uint32_t msg_id,
        std::span<const uint8_t> fbs_payload);

    /// Logout request handler (awaitable coroutine).
    /// Metadata accessed via current_meta_ (set before dispatch).
    boost::asio::awaitable<apex::core::Result<void>> handle_logout(
        apex::core::SessionPtr session,
        uint32_t msg_id,
        std::span<const uint8_t> fbs_payload);

    /// Refresh Token request handler (awaitable coroutine).
    /// Metadata accessed via current_meta_ (set before dispatch).
    boost::asio::awaitable<apex::core::Result<void>> handle_refresh_token(
        apex::core::SessionPtr session,
        uint32_t msg_id,
        std::span<const uint8_t> fbs_payload);

    /// Build response Kafka Envelope and produce to reply_topic (or fallback).
    void send_response(uint32_t msg_id,
                       uint64_t corr_id,
                       uint16_t core_id,
                       uint64_t session_id,
                       std::span<const uint8_t> fbs_payload,
                       const std::string& reply_topic);

    AuthConfig config_;
    apex::core::MessageDispatcher dispatcher_;     ///< O(1) hash map dispatch
    boost::asio::any_io_executor executor_;        ///< Kafka→coroutine bridge (injected from main)
    EnvelopeMetadata current_meta_;                ///< Cached metadata for current dispatch (side-channel)
    apex::shared::adapters::kafka::KafkaAdapter& kafka_;
    apex::shared::adapters::redis::RedisAdapter& redis_;
    apex::shared::adapters::pg::PgAdapter& pg_;

    JwtManager jwt_manager_;
    PasswordHasher password_hasher_;
    SessionStore session_store_;

    bool started_{false};
};

} // namespace apex::auth_svc
