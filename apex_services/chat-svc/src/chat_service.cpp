#include <apex/chat_svc/chat_service.hpp>

#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

// FlatBuffers generated headers
#include <chat_room_generated.h>
#include <chat_message_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <string>

namespace apex::chat_svc {

namespace envelope = apex::shared::protocols::kafka;
// ============================================================
// Construction / Lifecycle
// ============================================================

ChatService::ChatService(
    Config config,
    apex::shared::adapters::kafka::KafkaAdapter& kafka,
    apex::shared::adapters::redis::RedisAdapter& redis_data,
    apex::shared::adapters::redis::RedisAdapter& redis_pubsub,
    apex::shared::adapters::pg::PgAdapter& pg)
    : config_(std::move(config))
    , kafka_(kafka)
    , redis_data_(redis_data)
    , redis_pubsub_(redis_pubsub)
    , pg_(pg)
{}

ChatService::~ChatService() {
    if (started_) {
        stop();
    }
}

void ChatService::start() {
    spdlog::info("[ChatService] Starting...");

    // Register Kafka consumer callback
    kafka_.set_message_callback(
        [this](std::string_view topic, int32_t partition,
               std::span<const uint8_t> key,
               std::span<const uint8_t> payload,
               int64_t offset) -> apex::core::Result<void> {
            return on_kafka_message(topic, partition, key, payload, offset);
        });

    started_ = true;
    spdlog::info("[ChatService] Started. Consuming from: {}", config_.request_topic);
}

void ChatService::stop() {
    spdlog::info("[ChatService] Stopping...");
    started_ = false;
}

// ============================================================
// Helpers
// ============================================================

uint64_t ChatService::current_timestamp_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void ChatService::send_response(
    uint32_t msg_id,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload)
{
    send_response_with_flags(msg_id,
        envelope::routing_flags::DIRECTION_RESPONSE,
        corr_id, core_id, session_id, fbs_payload);
}

void ChatService::send_response_with_flags(
    uint32_t msg_id,
    uint16_t flags,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload)
{
    // Build RoutingHeader
    envelope::RoutingHeader routing;
    routing.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    routing.flags = flags;
    routing.msg_id = msg_id;

    // Build MetadataPrefix
    envelope::MetadataPrefix metadata;
    metadata.meta_version = envelope::MetadataPrefix::CURRENT_VERSION;
    metadata.core_id    = core_id;
    metadata.corr_id    = corr_id;
    metadata.source_id  = envelope::source_ids::CHAT;  // 2
    metadata.session_id = session_id;
    metadata.timestamp  = current_timestamp_ms();

    // Serialize: [Routing 8B] + [Metadata 32B] + [Payload NB]
    auto routing_bytes  = routing.serialize();
    auto metadata_bytes = metadata.serialize();

    std::vector<uint8_t> envelope_buf;
    envelope_buf.reserve(envelope::ENVELOPE_HEADER_SIZE + fbs_payload.size());
    envelope_buf.insert(envelope_buf.end(),
                        routing_bytes.begin(), routing_bytes.end());
    envelope_buf.insert(envelope_buf.end(),
                        metadata_bytes.begin(), metadata_bytes.end());
    envelope_buf.insert(envelope_buf.end(),
                        fbs_payload.begin(), fbs_payload.end());

    // Use session_id as Kafka key (design doc section 7.1)
    auto key = std::to_string(session_id);
    auto result = kafka_.produce(
        config_.response_topic,
        key,
        std::span<const uint8_t>(envelope_buf));

    if (!result.has_value()) {
        spdlog::error("[ChatService] Failed to produce response (msg_id: {}, corr_id: {})",
                      msg_id, corr_id);
    }
}

std::vector<uint8_t> ChatService::build_pubsub_payload(
    uint32_t msg_id,
    std::span<const uint8_t> fbs_payload) const
{
    // Format: [msg_id(u32 LE)] + [fbs payload]
    // Gateway reads msg_id to build WireHeader, then forwards fbs payload.
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(uint32_t) + fbs_payload.size());

    // Little-endian msg_id
    buf.push_back(static_cast<uint8_t>(msg_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 24) & 0xFF));

    buf.insert(buf.end(), fbs_payload.begin(), fbs_payload.end());
    return buf;
}

// ============================================================
// Kafka Message Dispatch
// ============================================================

apex::core::Result<void> ChatService::on_kafka_message(
    std::string_view topic,
    int32_t /*partition*/,
    std::span<const uint8_t> /*key*/,
    std::span<const uint8_t> payload,
    int64_t /*offset*/)
{
    if (topic != config_.request_topic) {
        return apex::core::ok();  // Ignore other topics
    }

    dispatch_envelope(payload);
    return apex::core::ok();
}

void ChatService::dispatch_envelope(std::span<const uint8_t> payload) {
    if (payload.size() < envelope::ENVELOPE_HEADER_SIZE) {
        spdlog::error("[ChatService] Envelope too small: {} bytes", payload.size());
        return;
    }

    // Parse RoutingHeader (8B)
    auto routing_result = envelope::RoutingHeader::parse(payload);
    if (!routing_result.has_value()) {
        spdlog::error("[ChatService] Failed to parse RoutingHeader");
        return;
    }

    // Parse MetadataPrefix (32B) starting at offset 8
    auto meta_result = envelope::MetadataPrefix::parse(
        payload.subspan(envelope::RoutingHeader::SIZE));
    if (!meta_result.has_value()) {
        spdlog::error("[ChatService] Failed to parse MetadataPrefix");
        return;
    }

    auto& routing = *routing_result;
    auto& metadata = *meta_result;

    // FlatBuffers payload starts at offset 40
    auto fbs_payload = payload.subspan(envelope::ENVELOPE_HEADER_SIZE);

    switch (routing.msg_id) {
    // Room management
    case msg_ids::CREATE_ROOM_REQUEST:
        handle_create_room(fbs_payload, metadata.corr_id,
                           metadata.core_id, metadata.session_id);
        break;
    case msg_ids::JOIN_ROOM_REQUEST:
        handle_join_room(fbs_payload, metadata.corr_id,
                         metadata.core_id, metadata.session_id);
        break;
    case msg_ids::LEAVE_ROOM_REQUEST:
        handle_leave_room(fbs_payload, metadata.corr_id,
                          metadata.core_id, metadata.session_id);
        break;
    case msg_ids::LIST_ROOMS_REQUEST:
        handle_list_rooms(fbs_payload, metadata.corr_id,
                          metadata.core_id, metadata.session_id);
        break;

    // Message send
    case msg_ids::SEND_MESSAGE_REQUEST:
        handle_send_message(fbs_payload, metadata.corr_id,
                            metadata.core_id, metadata.session_id);
        break;

    // Whisper
    case msg_ids::WHISPER_REQUEST:
        handle_whisper(fbs_payload, metadata.corr_id,
                       metadata.core_id, metadata.session_id);
        break;

    // History
    case msg_ids::CHAT_HISTORY_REQUEST:
        handle_chat_history(fbs_payload, metadata.corr_id,
                            metadata.core_id, metadata.session_id);
        break;

    // Global broadcast
    case msg_ids::GLOBAL_BROADCAST_REQUEST:
        handle_global_broadcast(fbs_payload, metadata.corr_id,
                                metadata.core_id, metadata.session_id);
        break;

    default:
        spdlog::warn("[ChatService] Unknown msg_id: {}", routing.msg_id);
        break;
    }
}

// ============================================================
// Room Management Handlers (Task 3)
// ============================================================

void ChatService::handle_create_room(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    // 1. FlatBuffers verify + parse
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::CreateRoomRequest>()) {
        spdlog::error("[ChatService] CreateRoomRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NAME_EMPTY);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::CreateRoomRequest>(fbs_payload.data());

    // 2. Input validation
    auto room_name = req->room_name();
    if (!room_name || room_name->size() == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NAME_EMPTY);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }
    if (room_name->size() > 100) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NAME_TOO_LONG);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }

    auto room_name_str = std::string(room_name->string_view());
    auto max_members = req->max_members() > 0 ? req->max_members() : config_.max_room_members;

    // NOTE: Full flow requires async coroutine for PG/Redis calls.
    // Kafka consumer callback is synchronous; in production, co_spawn a coroutine.
    // Current implementation is synchronous placeholder -- async integration
    // will be completed in E2E phase (Plan 5).

    spdlog::info("[ChatService] handle_create_room (name: {}, max: {}, corr_id: {}, session: {})",
                 room_name_str, max_members, corr_id, session_id);

    // Placeholder: send success response
    // Real implementation will:
    // 1. PG INSERT INTO chat_svc.chat_rooms RETURNING room_id
    // 2. Redis HSET chat:room:{id}:info + SADD members + user:rooms
    // 3. Send CreateRoomResponse with room_id

    flatbuffers::FlatBufferBuilder fbb(256);
    auto name_off = fbb.CreateString(room_name_str);
    auto resp = fbs::CreateCreateRoomResponse(fbb,
        fbs::ChatRoomError_NONE,
        0,   // room_id placeholder
        name_off);
    fbb.Finish(resp);
    send_response(msg_ids::CREATE_ROOM_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

void ChatService::handle_join_room(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::JoinRoomRequest>()) {
        spdlog::error("[ChatService] JoinRoomRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::JoinRoomRequest>(fbs_payload.data());
    auto room_id = req->room_id();

    spdlog::info("[ChatService] handle_join_room (room: {}, corr_id: {}, session: {})",
                 room_id, corr_id, session_id);

    // NOTE: Full flow (async):
    // 1. Redis EXISTS chat:room:{id}:info -> ROOM_NOT_FOUND check
    // 2. Redis SISMEMBER -> ALREADY_IN_ROOM check
    // 3. Redis SCARD -> ROOM_FULL check
    // 4. Redis SADD members + user:rooms
    // 5. Send subscribe control message to Gateway
    // 6. Send JoinRoomResponse

    flatbuffers::FlatBufferBuilder fbb(256);
    auto resp = fbs::CreateJoinRoomResponse(fbb,
        fbs::ChatRoomError_NONE,
        room_id,
        0,   // room_name placeholder
        0);  // member_count placeholder
    fbb.Finish(resp);
    send_response(msg_ids::JOIN_ROOM_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

void ChatService::handle_leave_room(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::LeaveRoomRequest>()) {
        spdlog::error("[ChatService] LeaveRoomRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::LeaveRoomRequest>(fbs_payload.data());
    auto room_id = req->room_id();

    spdlog::info("[ChatService] handle_leave_room (room: {}, corr_id: {}, session: {})",
                 room_id, corr_id, session_id);

    // NOTE: Full flow (async):
    // 1. Redis SISMEMBER -> NOT_IN_ROOM check
    // 2. Redis SREM members + user:rooms
    // 3. Send unsubscribe control message to Gateway
    // 4. Send LeaveRoomResponse

    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateLeaveRoomResponse(fbb,
        fbs::ChatRoomError_NONE,
        room_id);
    fbb.Finish(resp);
    send_response(msg_ids::LEAVE_ROOM_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

void ChatService::handle_list_rooms(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::ListRoomsRequest>()) {
        spdlog::error("[ChatService] ListRoomsRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::ListRoomsRequest>(fbs_payload.data());
    auto offset = req->offset();
    auto limit  = std::min(req->limit(), 100u);

    spdlog::info("[ChatService] handle_list_rooms (offset: {}, limit: {}, corr_id: {})",
                 offset, limit, corr_id);

    // NOTE: Full flow (async):
    // 1. PG SELECT from chat_svc.chat_rooms WHERE is_active
    // 2. For each room: Redis SCARD for real-time member count
    // 3. Build ListRoomsResponse with RoomInfo vector

    flatbuffers::FlatBufferBuilder fbb(256);
    auto resp = fbs::CreateListRoomsResponse(fbb,
        fbs::ChatRoomError_NONE,
        0,    // rooms vector placeholder
        0);   // total_count placeholder
    fbb.Finish(resp);
    send_response(msg_ids::LIST_ROOMS_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

// ============================================================
// Message Send + Redis Pub/Sub Broadcast (Task 4)
// ============================================================

void ChatService::handle_send_message(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::SendMessageRequest>()) {
        spdlog::error("[ChatService] SendMessageRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::SendMessageRequest>(fbs_payload.data());
    auto room_id = req->room_id();
    auto content = req->content();

    // 1. Input validation
    if (!content || content->size() == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateSendMessageResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::SEND_MESSAGE_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }
    if (content->size() > config_.max_message_length) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateSendMessageResponse(fbb,
            fbs::ChatMessageError_MESSAGE_TOO_LONG);
        fbb.Finish(resp);
        send_response(msg_ids::SEND_MESSAGE_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();

    spdlog::info("[ChatService] handle_send_message (room: {}, corr_id: {}, session: {})",
                 room_id, corr_id, session_id);

    // NOTE: Full flow (async):
    // 1. Redis SISMEMBER -> NOT_IN_ROOM check
    // 2. Redis PUBLISH pub:chat:room:{id} (real-time broadcast via Pub/Sub)
    //    payload = build_pubsub_payload(2013, ChatMessage FBS)
    // 3. Kafka produce to chat.messages.persist (room_id as key for ordering)
    //    -> ChatDbConsumer -> PostgreSQL INSERT
    // 4. Send SendMessageResponse

    // Real-time broadcast placeholder:
    // In production, this builds a ChatMessage FlatBuffer and publishes via Redis #3
    {
        flatbuffers::FlatBufferBuilder fbb_pub(512);
        auto sender_off  = fbb_pub.CreateString("placeholder_user");
        auto content_off = fbb_pub.CreateString(content_str);
        auto channel_off = fbb_pub.CreateString(std::format("pub:chat:room:{}", room_id));
        auto chat_msg = fbs::CreateChatMessage(fbb_pub,
            room_id, 0 /* sender_id placeholder */, sender_off, content_off,
            0 /* message_id TBD */, timestamp, channel_off);
        fbb_pub.Finish(chat_msg);

        // Redis PUBLISH would happen here
        // auto pub_payload = build_pubsub_payload(msg_ids::CHAT_MESSAGE,
        //     {fbb_pub.GetBufferPointer(), fbb_pub.GetSize()});
    }

    // History persistence placeholder:
    // In production, produce to chat.messages.persist topic
    {
        flatbuffers::FlatBufferBuilder fbb_db(512);
        auto sender_off  = fbb_db.CreateString("placeholder_user");
        auto content_off = fbb_db.CreateString(content_str);
        auto db_msg = fbs::CreateChatMessage(fbb_db,
            room_id, 0 /* sender_id */, sender_off, content_off,
            0, timestamp, 0);
        fbb_db.Finish(db_msg);

        kafka_.produce(config_.persist_topic,
            std::to_string(room_id),
            std::span<const uint8_t>(fbb_db.GetBufferPointer(), fbb_db.GetSize()));
    }

    // 5. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateSendMessageResponse(fbb,
        fbs::ChatMessageError_NONE, 0, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::SEND_MESSAGE_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

// ============================================================
// Whisper (Task 5)
// ============================================================

void ChatService::handle_whisper(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::WhisperRequest>()) {
        spdlog::error("[ChatService] WhisperRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::WhisperRequest>(fbs_payload.data());
    auto target_user_id = req->target_user_id();
    auto content = req->content();

    // 1. Input validation
    if (!content || content->size() == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateWhisperResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::WHISPER_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }
    if (content->size() > config_.max_message_length) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateWhisperResponse(fbb,
            fbs::ChatMessageError_MESSAGE_TOO_LONG);
        fbb.Finish(resp);
        send_response(msg_ids::WHISPER_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();

    spdlog::info("[ChatService] handle_whisper (target: {}, corr_id: {}, session: {})",
                 target_user_id, corr_id, session_id);

    // NOTE: Full flow (async):
    // 1. Redis GET session:user:{target_user_id} -> target_session_id
    //    -> TARGET_OFFLINE if not found
    // 2. Build WhisperMessage FBS
    // 3. send_response_with_flags(WHISPER_MESSAGE, unicast flags,
    //    corr_id=0, core_id=0, target_session_id, payload)
    //    -> Gateway delivers to target session
    // 4. Send WhisperResponse to sender

    // Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateWhisperResponse(fbb,
        fbs::ChatMessageError_NONE, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::WHISPER_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

// ============================================================
// Chat History (Task 6)
// ============================================================

void ChatService::handle_chat_history(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::ChatHistoryRequest>()) {
        spdlog::error("[ChatService] ChatHistoryRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::ChatHistoryRequest>(fbs_payload.data());
    auto room_id = req->room_id();
    auto before_message_id = req->before_message_id();
    auto limit = std::min(req->limit(), static_cast<uint32_t>(config_.history_page_size));

    spdlog::info("[ChatService] handle_chat_history (room: {}, before: {}, limit: {}, corr_id: {})",
                 room_id, before_message_id, limit, corr_id);

    // NOTE: Full flow (async):
    // 1. Redis SISMEMBER -> NOT_IN_ROOM check
    // 2. PG SELECT from chat_svc.chat_messages
    //    WHERE room_id = $1 [AND message_id < $2] ORDER BY created_at DESC LIMIT $N+1
    // 3. has_more = row_count > limit
    // 4. Build ChatHistoryResponse with HistoryMessage vector

    // Placeholder: empty history
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateChatHistoryResponse(fbb,
        fbs::ChatMessageError_NONE,
        0,     // messages vector placeholder
        false);
    fbb.Finish(resp);
    send_response(msg_ids::CHAT_HISTORY_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

// ============================================================
// Global Broadcast (Task 7)
// ============================================================

void ChatService::handle_global_broadcast(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id, uint16_t core_id, uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::GlobalBroadcastRequest>()) {
        spdlog::error("[ChatService] GlobalBroadcastRequest verification failed");
        return;
    }

    auto* req = flatbuffers::GetRoot<fbs::GlobalBroadcastRequest>(fbs_payload.data());
    auto content = req->content();

    // 1. Input validation
    if (!content || content->size() == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateGlobalBroadcastResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, corr_id, core_id, session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()});
        return;
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();

    spdlog::info("[ChatService] handle_global_broadcast (corr_id: {}, session: {})",
                 corr_id, session_id);

    // NOTE: Full flow (async):
    // 1. Permission check (Wave 2: all authenticated users for demo)
    // 2. Build GlobalChatMessage FBS
    // 3. build_pubsub_payload(2043, GlobalChatMessage, DELIVERY_BROADCAST flags)
    // 4. Redis #3 PUBLISH pub:global:chat
    //    -> All Gateway instances receive and fan-out to all sessions
    // 5. Send GlobalBroadcastResponse

    // Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateGlobalBroadcastResponse(fbb,
        fbs::ChatMessageError_NONE, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, corr_id, core_id, session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()});
}

} // namespace apex::chat_svc
