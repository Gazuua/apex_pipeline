// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/kafka_message_meta.hpp>
#include <apex/core/result.hpp>
#include <apex/core/service_base.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations
namespace apex::shared::adapters::kafka
{
class KafkaAdapter;
}
namespace apex::shared::adapters::redis
{
class RedisAdapter;
}
namespace apex::shared::adapters::pg
{
class PgAdapter;
}

// Forward declarations -- FlatBuffers generated types
namespace apex::chat_svc::fbs
{
struct CreateRoomRequest;
struct JoinRoomRequest;
struct LeaveRoomRequest;
struct ListRoomsRequest;
struct SendMessageRequest;
struct WhisperRequest;
struct ChatHistoryRequest;
struct GlobalBroadcastRequest;
} // namespace apex::chat_svc::fbs

namespace apex::chat_svc
{

/// Global broadcast channel name (Redis Pub/Sub).
inline constexpr std::string_view GLOBAL_CHAT_CHANNEL = "pub:global:chat";

/// Chat Service -- Server+ServiceBase 패턴, kafka_route 기반.
///
/// Kafka envelope metadata(MetadataPrefix)를 핸들러 인자로 직접 전달.
/// KafkaDispatchBridge가 파싱/디스패치를 처리.
///
/// Adapter dependencies:
///   - KafkaAdapter: consumer(chat.requests), producer(chat.responses, chat.messages.persist)
///   - RedisAdapter("data"): Room membership, online status (chat:room:{id}:members Set)
///   - RedisAdapter("pubsub"): Pub/Sub broadcast (pub:chat:room:{id}, pub:global:chat)
///   - PgAdapter: History storage/query (chat_svc schema)
class ChatService : public apex::core::ServiceBase<ChatService>
{
  public:
    struct Config
    {
        std::string request_topic = "chat.requests";
        std::string response_topic = "chat.responses";
        std::string persist_topic = "chat.messages.persist";
        uint32_t max_room_members = 100;
        uint32_t max_message_length = 2000; // bytes
        uint32_t history_page_size = 50;
        size_t max_room_name_length = 100;
        uint32_t max_list_rooms_limit = 100;
    };

    explicit ChatService(Config config);

    /// Phase 1: 어댑터 참조 취득.
    void on_configure(apex::core::ConfigureContext& ctx) override;

    /// kafka_route 등록.
    void on_start() override;

    /// 정리 로직.
    void on_stop() override;

  private:
    // --- msg_id constants (from msg_registry.toml) ---
    struct msg_ids
    {
        // Room management
        static constexpr uint32_t CREATE_ROOM_REQUEST = 2001;
        static constexpr uint32_t CREATE_ROOM_RESPONSE = 2002;
        static constexpr uint32_t JOIN_ROOM_REQUEST = 2003;
        static constexpr uint32_t JOIN_ROOM_RESPONSE = 2004;
        static constexpr uint32_t LEAVE_ROOM_REQUEST = 2005;
        static constexpr uint32_t LEAVE_ROOM_RESPONSE = 2006;
        static constexpr uint32_t LIST_ROOMS_REQUEST = 2007;
        static constexpr uint32_t LIST_ROOMS_RESPONSE = 2008;

        // Message send/broadcast
        static constexpr uint32_t SEND_MESSAGE_REQUEST = 2011;
        static constexpr uint32_t SEND_MESSAGE_RESPONSE = 2012;
        static constexpr uint32_t CHAT_MESSAGE = 2013;

        // Whisper (1:1)
        static constexpr uint32_t WHISPER_REQUEST = 2021;
        static constexpr uint32_t WHISPER_RESPONSE = 2022;
        static constexpr uint32_t WHISPER_MESSAGE = 2023;

        // History
        static constexpr uint32_t CHAT_HISTORY_REQUEST = 2031;
        static constexpr uint32_t CHAT_HISTORY_RESPONSE = 2032;

        // Global broadcast
        static constexpr uint32_t GLOBAL_BROADCAST_REQUEST = 2041;
        static constexpr uint32_t GLOBAL_BROADCAST_RESPONSE = 2042;
        static constexpr uint32_t GLOBAL_CHAT_MESSAGE = 2043;
    };

    // --- Handlers (awaitable coroutines, MetadataPrefix 인자로 직접 수신) ---

    // Room management
    boost::asio::awaitable<apex::core::Result<void>> on_create_room(const apex::core::KafkaMessageMeta& meta,
                                                                    uint32_t msg_id, const fbs::CreateRoomRequest* req);
    boost::asio::awaitable<apex::core::Result<void>> on_join_room(const apex::core::KafkaMessageMeta& meta,
                                                                  uint32_t msg_id, const fbs::JoinRoomRequest* req);
    boost::asio::awaitable<apex::core::Result<void>> on_leave_room(const apex::core::KafkaMessageMeta& meta,
                                                                   uint32_t msg_id, const fbs::LeaveRoomRequest* req);
    boost::asio::awaitable<apex::core::Result<void>> on_list_rooms(const apex::core::KafkaMessageMeta& meta,
                                                                   uint32_t msg_id, const fbs::ListRoomsRequest* req);

    // Message send
    boost::asio::awaitable<apex::core::Result<void>>
    on_send_message(const apex::core::KafkaMessageMeta& meta, uint32_t msg_id, const fbs::SendMessageRequest* req);

    // Whisper (1:1)
    boost::asio::awaitable<apex::core::Result<void>> on_whisper(const apex::core::KafkaMessageMeta& meta,
                                                                uint32_t msg_id, const fbs::WhisperRequest* req);

    // History
    boost::asio::awaitable<apex::core::Result<void>>
    on_chat_history(const apex::core::KafkaMessageMeta& meta, uint32_t msg_id, const fbs::ChatHistoryRequest* req);

    // Global broadcast
    boost::asio::awaitable<apex::core::Result<void>> on_global_broadcast(const apex::core::KafkaMessageMeta& meta,
                                                                         uint32_t msg_id,
                                                                         const fbs::GlobalBroadcastRequest* req);

    // --- Helpers ---

    /// Build Kafka Envelope and produce to reply_topic (or fallback to config).
    void send_response(uint32_t msg_id, uint64_t corr_id, uint16_t core_id, uint64_t session_id,
                       std::span<const uint8_t> fbs_payload, const std::string& reply_topic);

    /// Build Kafka Envelope with custom flags and produce to reply_topic (or fallback).
    void send_response_with_flags(uint32_t msg_id, uint16_t flags, uint64_t corr_id, uint16_t core_id,
                                  uint64_t session_id, std::span<const uint8_t> fbs_payload,
                                  const std::string& reply_topic);

    /// Build Redis Pub/Sub payload: [msg_id(u32 BE)] + [fbs payload]
    [[nodiscard]] std::vector<uint8_t> build_pubsub_payload(uint32_t msg_id,
                                                            std::span<const uint8_t> fbs_payload) const;

    /// Get current timestamp in milliseconds.
    [[nodiscard]] static uint64_t current_timestamp_ms() noexcept;

    // ── 에러 응답 헬퍼 [D5] ──────────────────────────────────────────────

    /// CreateRoomResponse 에러.
    boost::asio::awaitable<apex::core::Result<void>> send_create_room_error(const apex::core::KafkaMessageMeta& meta,
                                                                            uint16_t error);

    /// JoinRoomResponse 에러 (room_id 포함).
    boost::asio::awaitable<apex::core::Result<void>> send_join_room_error(const apex::core::KafkaMessageMeta& meta,
                                                                          uint16_t error, uint64_t room_id);

    /// LeaveRoomResponse 에러 (room_id 포함).
    boost::asio::awaitable<apex::core::Result<void>> send_leave_room_error(const apex::core::KafkaMessageMeta& meta,
                                                                           uint16_t error, uint64_t room_id);

    /// ListRoomsResponse 에러.
    boost::asio::awaitable<apex::core::Result<void>> send_list_rooms_error(const apex::core::KafkaMessageMeta& meta,
                                                                           uint16_t error);

    /// SendMessageResponse 에러.
    boost::asio::awaitable<apex::core::Result<void>> send_message_error(const apex::core::KafkaMessageMeta& meta,
                                                                        uint16_t error);

    /// WhisperResponse 에러.
    boost::asio::awaitable<apex::core::Result<void>> send_whisper_error(const apex::core::KafkaMessageMeta& meta,
                                                                        uint16_t error);

    /// ChatHistoryResponse 에러.
    boost::asio::awaitable<apex::core::Result<void>> send_history_error(const apex::core::KafkaMessageMeta& meta,
                                                                        uint16_t error);

    /// GlobalBroadcastResponse 에러.
    boost::asio::awaitable<apex::core::Result<void>>
    send_global_broadcast_error(const apex::core::KafkaMessageMeta& meta, uint16_t error);

    Config config_;
    apex::shared::adapters::kafka::KafkaAdapter* kafka_{nullptr};
    apex::shared::adapters::redis::RedisAdapter* redis_data_{nullptr};
    apex::shared::adapters::redis::RedisAdapter* redis_pubsub_{nullptr};
    apex::shared::adapters::pg::PgAdapter* pg_{nullptr};
};

} // namespace apex::chat_svc
