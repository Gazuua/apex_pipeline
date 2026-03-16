#pragma once

#include <apex/core/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations
namespace apex::shared::adapters::kafka { class KafkaAdapter; }
namespace apex::shared::adapters::redis { class RedisAdapter; }
namespace apex::shared::adapters::pg    { class PgAdapter; }

namespace apex::chat_svc {

/// Chat Service -- Kafka-based room management, message broadcast, chat history.
///
/// Consumes from `chat.requests` topic, processes room/message/whisper/history,
/// and produces responses to `chat.responses` topic. Real-time broadcast via
/// Redis Pub/Sub (pub:chat:room:{id}, pub:global:chat). History persistence
/// via Kafka `chat.messages.persist` topic -> ChatDbConsumer -> PostgreSQL.
///
/// Does NOT inherit ServiceBase -- Kafka message-driven, not HTTP session-driven.
/// (Same pattern as AuthService)
///
/// Adapter dependencies:
///   - KafkaAdapter: consumer(chat.requests), producer(chat.responses, chat.messages.persist)
///   - RedisAdapter(#2): Room membership, online status (chat:room:{id}:members Set)
///   - RedisAdapter(#3): Pub/Sub broadcast (pub:chat:room:{id}, pub:global:chat)
///   - PgAdapter: History storage/query (chat_svc schema)
class ChatService {
public:
    struct Config {
        std::string request_topic         = "chat.requests";
        std::string response_topic        = "chat.responses";
        std::string persist_topic         = "chat.messages.persist";
        uint32_t    max_room_members      = 100;
        uint32_t    max_message_length    = 2000;  // bytes
        uint32_t    history_page_size     = 50;
    };

    explicit ChatService(
        Config config,
        apex::shared::adapters::kafka::KafkaAdapter& kafka,
        apex::shared::adapters::redis::RedisAdapter& redis_data,
        apex::shared::adapters::redis::RedisAdapter& redis_pubsub,
        apex::shared::adapters::pg::PgAdapter& pg);

    ~ChatService();

    /// Start service: register Kafka consumer callback
    void start();

    /// Stop service
    void stop();

    [[nodiscard]] std::string_view name() const noexcept { return "chat"; }
    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    // --- msg_id constants (from msg_registry.toml) ---
    struct msg_ids {
        // Room management
        static constexpr uint32_t CREATE_ROOM_REQUEST   = 2001;
        static constexpr uint32_t CREATE_ROOM_RESPONSE  = 2002;
        static constexpr uint32_t JOIN_ROOM_REQUEST     = 2003;
        static constexpr uint32_t JOIN_ROOM_RESPONSE    = 2004;
        static constexpr uint32_t LEAVE_ROOM_REQUEST    = 2005;
        static constexpr uint32_t LEAVE_ROOM_RESPONSE   = 2006;
        static constexpr uint32_t LIST_ROOMS_REQUEST    = 2007;
        static constexpr uint32_t LIST_ROOMS_RESPONSE   = 2008;

        // Message send/broadcast
        static constexpr uint32_t SEND_MESSAGE_REQUEST  = 2011;
        static constexpr uint32_t SEND_MESSAGE_RESPONSE = 2012;
        static constexpr uint32_t CHAT_MESSAGE          = 2013;

        // Whisper (1:1)
        static constexpr uint32_t WHISPER_REQUEST       = 2021;
        static constexpr uint32_t WHISPER_RESPONSE      = 2022;
        static constexpr uint32_t WHISPER_MESSAGE       = 2023;

        // History
        static constexpr uint32_t CHAT_HISTORY_REQUEST  = 2031;
        static constexpr uint32_t CHAT_HISTORY_RESPONSE = 2032;

        // Global broadcast
        static constexpr uint32_t GLOBAL_BROADCAST_REQUEST  = 2041;
        static constexpr uint32_t GLOBAL_BROADCAST_RESPONSE = 2042;
        static constexpr uint32_t GLOBAL_CHAT_MESSAGE       = 2043;
    };

    /// Kafka message receive callback
    apex::core::Result<void> on_kafka_message(
        std::string_view topic,
        int32_t partition,
        std::span<const uint8_t> key,
        std::span<const uint8_t> payload,
        int64_t offset);

    /// Parse Kafka Envelope (RoutingHeader + Metadata) and dispatch by msg_id
    void dispatch_envelope(std::span<const uint8_t> payload);

    // --- Handlers (receive fbs_payload + envelope metadata) ---

    // Room management (Task 3)
    void handle_create_room(std::span<const uint8_t> fbs_payload,
                            uint64_t corr_id, uint16_t core_id, uint64_t session_id);
    void handle_join_room(std::span<const uint8_t> fbs_payload,
                          uint64_t corr_id, uint16_t core_id, uint64_t session_id);
    void handle_leave_room(std::span<const uint8_t> fbs_payload,
                           uint64_t corr_id, uint16_t core_id, uint64_t session_id);
    void handle_list_rooms(std::span<const uint8_t> fbs_payload,
                           uint64_t corr_id, uint16_t core_id, uint64_t session_id);

    // Message send (Task 4)
    void handle_send_message(std::span<const uint8_t> fbs_payload,
                             uint64_t corr_id, uint16_t core_id, uint64_t session_id);

    // Whisper (Task 5)
    void handle_whisper(std::span<const uint8_t> fbs_payload,
                        uint64_t corr_id, uint16_t core_id, uint64_t session_id);

    // History (Task 6)
    void handle_chat_history(std::span<const uint8_t> fbs_payload,
                             uint64_t corr_id, uint16_t core_id, uint64_t session_id);

    // Global broadcast (Task 7)
    void handle_global_broadcast(std::span<const uint8_t> fbs_payload,
                                 uint64_t corr_id, uint16_t core_id, uint64_t session_id);

    // --- Helpers ---

    /// Build Kafka Envelope and produce to response topic.
    void send_response(uint32_t msg_id,
                       uint64_t corr_id,
                       uint16_t core_id,
                       uint64_t session_id,
                       std::span<const uint8_t> fbs_payload);

    /// Build Kafka Envelope with custom flags and produce to response topic.
    void send_response_with_flags(uint32_t msg_id,
                                  uint16_t flags,
                                  uint64_t corr_id,
                                  uint16_t core_id,
                                  uint64_t session_id,
                                  std::span<const uint8_t> fbs_payload);

    /// Build Redis Pub/Sub payload: [msg_id(u32 BE)] + [fbs payload]
    [[nodiscard]] std::vector<uint8_t> build_pubsub_payload(
        uint32_t msg_id,
        std::span<const uint8_t> fbs_payload) const;

    /// Get current timestamp in milliseconds.
    [[nodiscard]] static uint64_t current_timestamp_ms() noexcept;

    Config config_;
    apex::shared::adapters::kafka::KafkaAdapter& kafka_;
    apex::shared::adapters::redis::RedisAdapter& redis_data_;
    apex::shared::adapters::redis::RedisAdapter& redis_pubsub_;
    apex::shared::adapters::pg::PgAdapter& pg_;

    bool started_{false};
};

} // namespace apex::chat_svc
