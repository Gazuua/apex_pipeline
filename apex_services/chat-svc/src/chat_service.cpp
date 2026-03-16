#include <apex/chat_svc/chat_service.hpp>

#include <apex/core/core_engine.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

// FlatBuffers generated headers
#include <chat_room_generated.h>
#include <chat_message_generated.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <array>
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
    boost::asio::any_io_executor executor,
    apex::shared::adapters::kafka::KafkaAdapter& kafka,
    apex::shared::adapters::redis::RedisAdapter& redis_data,
    apex::shared::adapters::redis::RedisAdapter& redis_pubsub,
    apex::shared::adapters::pg::PgAdapter& pg)
    : config_(std::move(config))
    , executor_(std::move(executor))
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

    // Register actual handlers to MessageDispatcher (O(1) hash map).
    // Handlers access Kafka envelope metadata via this->current_meta_
    // (set in dispatch_envelope() before dispatcher_.dispatch()).
    dispatcher_.register_handler(msg_ids::CREATE_ROOM_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_create_room(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::JOIN_ROOM_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_join_room(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::LEAVE_ROOM_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_leave_room(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::LIST_ROOMS_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_list_rooms(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::SEND_MESSAGE_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_send_message(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::WHISPER_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_whisper(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::CHAT_HISTORY_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_chat_history(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::GLOBAL_BROADCAST_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_global_broadcast(std::move(session), msg_id, payload);
        });

    // Register Kafka consumer callback
    kafka_.set_message_callback(
        [this](std::string_view topic, int32_t partition,
               std::span<const uint8_t> key,
               std::span<const uint8_t> payload,
               int64_t offset) -> apex::core::Result<void> {
            return on_kafka_message(topic, partition, key, payload, offset);
        });

    started_ = true;
    spdlog::info("[ChatService] Started. {} handlers registered. Consuming from: {}",
                 dispatcher_.handler_count(), config_.request_topic);
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
    std::span<const uint8_t> fbs_payload,
    const std::string& reply_topic)
{
    send_response_with_flags(msg_id,
        envelope::routing_flags::DIRECTION_RESPONSE,
        corr_id, core_id, session_id, fbs_payload, reply_topic);
}

void ChatService::send_response_with_flags(
    uint32_t msg_id,
    uint16_t flags,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload,
    const std::string& reply_topic)
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

    // 응답에는 reply_topic 불포함 — 빈 문자열로 build_full_envelope 호출
    auto envelope_buf = envelope::build_full_envelope(routing, metadata, "", fbs_payload);

    // Reply-To: reply_topic이 있으면 그쪽으로 응답, 없으면 fallback
    const auto& target_topic = reply_topic.empty()
        ? config_.response_topic : reply_topic;

    // Use session_id as Kafka key (design doc section 7.1)
    auto key = std::to_string(session_id);
    auto result = kafka_.produce(
        target_topic,
        key,
        std::span<const uint8_t>(envelope_buf));

    if (!result.has_value()) {
        spdlog::error("[ChatService] Failed to produce response to '{}' (msg_id: {}, corr_id: {})",
                      target_topic, msg_id, corr_id);
    }
}

std::vector<uint8_t> ChatService::build_pubsub_payload(
    uint32_t msg_id,
    std::span<const uint8_t> fbs_payload) const
{
    // Format: [msg_id(u32 BE)] + [fbs payload]
    // Gateway BroadcastFanout reads msg_id as big-endian to build WireHeader.
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(uint32_t) + fbs_payload.size());

    // Big-endian msg_id (matches BroadcastFanout::build_wire_frame parsing)
    buf.push_back(static_cast<uint8_t>((msg_id >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(msg_id & 0xFF));

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

    // Parse MetadataPrefix (40B) starting at offset 8
    auto meta_result = envelope::MetadataPrefix::parse(
        payload.subspan(envelope::RoutingHeader::SIZE));
    if (!meta_result.has_value()) {
        spdlog::error("[ChatService] Failed to parse MetadataPrefix");
        return;
    }

    auto& routing = *routing_result;
    auto& metadata = *meta_result;

    // Check if handler exists (O(1) lookup via MessageDispatcher)
    if (!dispatcher_.has_handler(routing.msg_id)) {
        spdlog::warn("[ChatService] Unknown msg_id: {}", routing.msg_id);
        return;
    }

    // Extract reply_topic (Reply-To 헤더 패턴)
    auto reply_topic = envelope::extract_reply_topic(routing.flags, payload);

    // FlatBuffers payload — reply_topic 존재 시 오프셋이 달라짐
    auto payload_offset = envelope::envelope_payload_offset(routing.flags, payload);
    auto fbs_payload = payload.subspan(payload_offset);

    // Build metadata locally for value capture into co_spawn lambda.
    // current_meta_ is NOT set here — each coroutine owns its own copy
    // to prevent data races when multiple coroutines are in flight.
    EnvelopeMetadata meta;
    meta.corr_id = metadata.corr_id;
    meta.core_id = metadata.core_id;
    meta.session_id = metadata.session_id;
    meta.user_id = metadata.user_id;
    meta.reply_topic = std::move(reply_topic);

    // Copy payload for coroutine lifetime safety.
    // Kafka callback's payload span is only valid during this synchronous call.
    // The coroutine may outlive the Kafka callback, so we must own the data.
    auto fbs_data = std::vector<uint8_t>(fbs_payload.begin(), fbs_payload.end());
    auto msg_id = routing.msg_id;

    // Kafka callback is synchronous — bridge to coroutine via co_spawn.
    // Metadata is value-captured (moved) into the lambda to avoid data races.
    // Each coroutine sets current_meta_ at its start for handler access.
    boost::asio::co_spawn(executor_,
        [this, msg_id, fbs_data = std::move(fbs_data),
         meta = std::move(meta)]() mutable
            -> boost::asio::awaitable<void> {
            current_meta_ = std::move(meta);
            auto payload_span = std::span<const uint8_t>(fbs_data);
            co_await dispatcher_.dispatch(nullptr, msg_id, payload_span);
        },
        boost::asio::detached);
}

// ============================================================
// Room Management Handlers (Task 3)
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_create_room(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    // 1. FlatBuffers verify + parse
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::CreateRoomRequest>()) {
        spdlog::error("[ChatService] CreateRoomRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NAME_EMPTY);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<fbs::CreateRoomRequest>(fbs_payload.data());

    // 2. Input validation
    auto room_name = req->room_name();
    if (!room_name || room_name->size() == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NAME_EMPTY);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }
    if (room_name->size() > 100) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NAME_TOO_LONG);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto room_name_str = std::string(room_name->string_view());
    auto max_members = req->max_members() > 0 ? req->max_members() : config_.max_room_members;
    auto user_id = meta.user_id;

    spdlog::info("[ChatService] handle_create_room (name: {}, max: {}, corr_id: {}, session: {})",
                 room_name_str, max_members, meta.corr_id, meta.session_id);

    // 1. PG INSERT — create room record
    std::array<std::string, 3> insert_params = {
        room_name_str,
        std::to_string(max_members),
        std::to_string(user_id)
    };
    auto pg_result = co_await pg_.query(
        "INSERT INTO chat_svc.chat_rooms (room_name, max_members, owner_id) "
        "VALUES ($1, $2::int, $3::bigint) RETURNING id",
        insert_params);

    if (!pg_result.has_value() || pg_result->row_count() == 0) {
        spdlog::error("[ChatService] PG INSERT chat_rooms failed: {}",
                      pg_result.has_value() ? "no rows returned"
                          : apex::core::error_code_name(pg_result.error()));
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateCreateRoomResponse(fbb,
            fbs::ChatRoomError_PERMISSION_DENIED);
        fbb.Finish(resp);
        send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto room_id = static_cast<uint64_t>(
        std::stoull(std::string(pg_result->value(0, 0))));

    // 2. Redis SADD — add owner as first member
    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);
    auto redis_result = co_await redis_data_.multiplexer(core_id).command(
        "SADD %s %s", members_key.c_str(), user_id_str.c_str());
    if (!redis_result.has_value()) {
        spdlog::warn("[ChatService] Redis SADD members failed for room {}", room_id);
        // Non-fatal: room created in PG, membership will be eventually consistent
    }

    // 3. Send CreateRoomResponse with actual room_id
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name_off = fbb.CreateString(room_name_str);
    auto resp = fbs::CreateCreateRoomResponse(fbb,
        fbs::ChatRoomError_NONE,
        room_id,
        name_off);
    fbb.Finish(resp);
    send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_join_room(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::JoinRoomRequest>()) {
        spdlog::error("[ChatService] JoinRoomRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateJoinRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NOT_FOUND);
        fbb.Finish(resp);
        send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<fbs::JoinRoomRequest>(fbs_payload.data());
    auto room_id = req->room_id();
    auto user_id = meta.user_id;

    spdlog::info("[ChatService] handle_join_room (room: {}, corr_id: {}, session: {})",
                 room_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);

    // 1. Check room existence in PG
    std::array<std::string, 1> room_params = {std::to_string(room_id)};
    auto room_result = co_await pg_.query(
        "SELECT room_name, max_members FROM chat_svc.chat_rooms "
        "WHERE id = $1::bigint AND is_active = true",
        room_params);

    if (!room_result.has_value() || room_result->row_count() == 0) {
        spdlog::warn("[ChatService] join_room: room {} not found", room_id);
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateJoinRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NOT_FOUND, room_id);
        fbb.Finish(resp);
        send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto room_name_sv = room_result->value(0, 0);
    auto max_members = static_cast<uint32_t>(
        std::stoul(std::string(room_result->value(0, 1))));

    // 2. SISMEMBER — already in room?
    auto is_member = co_await redis_data_.multiplexer(core_id).command(
        "SISMEMBER %s %s", members_key.c_str(), user_id_str.c_str());
    if (is_member.has_value() && is_member->integer == 1) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateJoinRoomResponse(fbb,
            fbs::ChatRoomError_ALREADY_IN_ROOM, room_id);
        fbb.Finish(resp);
        send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // 3. SCARD — room full?
    auto card_result = co_await redis_data_.multiplexer(core_id).command(
        "SCARD %s", members_key.c_str());
    if (card_result.has_value() &&
        static_cast<uint32_t>(card_result->integer) >= max_members) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateJoinRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_FULL, room_id);
        fbb.Finish(resp);
        send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // 4. SADD — add member
    auto sadd_result = co_await redis_data_.multiplexer(core_id).command(
        "SADD %s %s", members_key.c_str(), user_id_str.c_str());
    if (!sadd_result.has_value()) {
        spdlog::warn("[ChatService] Redis SADD failed for room {}", room_id);
    }

    // 5. Get updated member count
    auto new_card = co_await redis_data_.multiplexer(core_id).command(
        "SCARD %s", members_key.c_str());
    auto member_count = static_cast<uint32_t>(
        new_card.has_value() ? new_card->integer : 0);

    // 6. Send JoinRoomResponse
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name_off = fbb.CreateString(room_name_sv.data(), room_name_sv.size());
    auto resp = fbs::CreateJoinRoomResponse(fbb,
        fbs::ChatRoomError_NONE,
        room_id,
        name_off,
        member_count);
    fbb.Finish(resp);
    send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_leave_room(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::LeaveRoomRequest>()) {
        spdlog::error("[ChatService] LeaveRoomRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateLeaveRoomResponse(fbb,
            fbs::ChatRoomError_ROOM_NOT_FOUND);
        fbb.Finish(resp);
        send_response(msg_ids::LEAVE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<fbs::LeaveRoomRequest>(fbs_payload.data());
    auto room_id = req->room_id();
    auto user_id = meta.user_id;

    spdlog::info("[ChatService] handle_leave_room (room: {}, corr_id: {}, session: {})",
                 room_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);

    // 1. SISMEMBER — check if user is in the room
    auto is_member = co_await redis_data_.multiplexer(core_id).command(
        "SISMEMBER %s %s", members_key.c_str(), user_id_str.c_str());
    if (!is_member.has_value() || is_member->integer == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateLeaveRoomResponse(fbb,
            fbs::ChatRoomError_NOT_IN_ROOM, room_id);
        fbb.Finish(resp);
        send_response(msg_ids::LEAVE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // 2. SREM — remove member
    auto srem_result = co_await redis_data_.multiplexer(core_id).command(
        "SREM %s %s", members_key.c_str(), user_id_str.c_str());
    if (!srem_result.has_value()) {
        spdlog::warn("[ChatService] Redis SREM failed for room {}", room_id);
    }

    // 3. Send LeaveRoomResponse
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateLeaveRoomResponse(fbb,
        fbs::ChatRoomError_NONE,
        room_id);
    fbb.Finish(resp);
    send_response(msg_ids::LEAVE_ROOM_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_list_rooms(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::ListRoomsRequest>()) {
        spdlog::error("[ChatService] ListRoomsRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateListRoomsResponse(fbb,
            fbs::ChatRoomError_ROOM_NOT_FOUND);
        fbb.Finish(resp);
        send_response(msg_ids::LIST_ROOMS_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<fbs::ListRoomsRequest>(fbs_payload.data());
    auto offset = req->offset();
    auto limit  = std::min(req->limit(), 100u);

    spdlog::info("[ChatService] handle_list_rooms (offset: {}, limit: {}, corr_id: {})",
                 offset, limit, meta.corr_id);

    // 1. PG: Get total count
    auto count_result = co_await pg_.query(
        "SELECT COUNT(*) FROM chat_svc.chat_rooms WHERE is_active = true");

    uint32_t total_count = 0;
    if (count_result.has_value() && count_result->row_count() > 0) {
        total_count = static_cast<uint32_t>(
            std::stoul(std::string(count_result->value(0, 0))));
    }

    // 2. PG: Get rooms page
    std::array<std::string, 2> page_params = {
        std::to_string(limit),
        std::to_string(offset)
    };
    auto rooms_result = co_await pg_.query(
        "SELECT id, room_name, max_members, owner_id "
        "FROM chat_svc.chat_rooms WHERE is_active = true "
        "ORDER BY id ASC LIMIT $1::int OFFSET $2::int",
        page_params);

    if (!rooms_result.has_value()) {
        spdlog::error("[ChatService] PG query chat_rooms failed: {}",
                      apex::core::error_code_name(rooms_result.error()));
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateListRoomsResponse(fbb,
            fbs::ChatRoomError_ROOM_NOT_FOUND);
        fbb.Finish(resp);
        send_response(msg_ids::LIST_ROOMS_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // 3. Build RoomInfo vector with real-time member count from Redis
    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& pg_res = *rooms_result;

    flatbuffers::FlatBufferBuilder fbb(512);
    std::vector<flatbuffers::Offset<fbs::RoomInfo>> room_offsets;
    room_offsets.reserve(static_cast<size_t>(pg_res.row_count()));

    for (int i = 0; i < pg_res.row_count(); ++i) {
        auto rid = static_cast<uint64_t>(
            std::stoull(std::string(pg_res.value(i, 0))));
        auto rname = pg_res.value(i, 1);
        auto rmax = static_cast<uint32_t>(
            std::stoul(std::string(pg_res.value(i, 2))));
        auto rowner = static_cast<uint64_t>(
            std::stoull(std::string(pg_res.value(i, 3))));

        // Redis SCARD for real-time member count
        auto members_key = std::format("chat:room:{}:members", rid);
        uint32_t member_count = 0;
        auto scard = co_await redis_data_.multiplexer(core_id).command(
            "SCARD %s", members_key.c_str());
        if (scard.has_value()) {
            member_count = static_cast<uint32_t>(scard->integer);
        }

        auto name_off = fbb.CreateString(rname.data(), rname.size());
        room_offsets.push_back(fbs::CreateRoomInfo(fbb,
            rid, name_off, member_count, rmax, rowner));
    }

    auto rooms_vec = fbb.CreateVector(room_offsets);
    auto resp = fbs::CreateListRoomsResponse(fbb,
        fbs::ChatRoomError_NONE,
        rooms_vec,
        total_count);
    fbb.Finish(resp);
    send_response(msg_ids::LIST_ROOMS_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

// ============================================================
// Message Send + Redis Pub/Sub Broadcast (Task 4)
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_send_message(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::SendMessageRequest>()) {
        spdlog::error("[ChatService] SendMessageRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateSendMessageResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
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
        send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }
    if (content->size() > config_.max_message_length) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateSendMessageResponse(fbb,
            fbs::ChatMessageError_MESSAGE_TOO_LONG);
        fbb.Finish(resp);
        send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();
    auto user_id = meta.user_id;

    spdlog::info("[ChatService] handle_send_message (room: {}, corr_id: {}, session: {})",
                 room_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);

    // 1. SISMEMBER — membership check
    auto is_member = co_await redis_data_.multiplexer(core_id).command(
        "SISMEMBER %s %s", members_key.c_str(), user_id_str.c_str());
    if (!is_member.has_value() || is_member->integer == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateSendMessageResponse(fbb,
            fbs::ChatMessageError_NOT_IN_ROOM);
        fbb.Finish(resp);
        send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // 2. Redis PUBLISH — real-time broadcast via pub/sub
    auto channel = std::format("pub:chat:room:{}", room_id);
    {
        flatbuffers::FlatBufferBuilder fbb_pub(512);
        auto sender_off  = fbb_pub.CreateString(user_id_str);
        auto content_off = fbb_pub.CreateString(content_str);
        auto channel_off = fbb_pub.CreateString(channel);
        auto chat_msg = fbs::CreateChatMessage(fbb_pub,
            room_id, user_id, sender_off, content_off,
            0 /* message_id assigned by DB */, timestamp, channel_off);
        fbb_pub.Finish(chat_msg);

        auto pub_payload = build_pubsub_payload(msg_ids::CHAT_MESSAGE,
            {fbb_pub.GetBufferPointer(), fbb_pub.GetSize()});
        auto pub_result = co_await redis_pubsub_.multiplexer(core_id).command(
            "PUBLISH %s %b", channel.c_str(),
            reinterpret_cast<const char*>(pub_payload.data()),
            static_cast<size_t>(pub_payload.size()));
        if (!pub_result.has_value()) {
            spdlog::warn("[ChatService] Redis PUBLISH failed for room {}", room_id);
        }
    }

    // 3. Kafka produce to persist topic for async DB storage
    {
        flatbuffers::FlatBufferBuilder fbb_db(512);
        auto sender_off  = fbb_db.CreateString(user_id_str);
        auto content_off = fbb_db.CreateString(content_str);
        auto db_msg = fbs::CreateChatMessage(fbb_db,
            room_id, user_id, sender_off, content_off,
            0, timestamp, 0);
        fbb_db.Finish(db_msg);

        kafka_.produce(config_.persist_topic,
            std::to_string(room_id),
            std::span<const uint8_t>(fbb_db.GetBufferPointer(), fbb_db.GetSize()));
    }

    // 4. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateSendMessageResponse(fbb,
        fbs::ChatMessageError_NONE, 0, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

// ============================================================
// Whisper (Task 5)
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_whisper(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::WhisperRequest>()) {
        spdlog::error("[ChatService] WhisperRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateWhisperResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
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
        send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }
    if (content->size() > config_.max_message_length) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateWhisperResponse(fbb,
            fbs::ChatMessageError_MESSAGE_TOO_LONG);
        fbb.Finish(resp);
        send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();
    auto sender_id = meta.user_id;

    spdlog::info("[ChatService] handle_whisper (target: {}, corr_id: {}, session: {})",
                 target_user_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();

    // 1. Redis GET — lookup target user's session_id for online check
    auto session_key = std::format("session:user:{}", target_user_id);
    auto session_result = co_await redis_data_.multiplexer(core_id).command(
        "GET %s", session_key.c_str());

    if (!session_result.has_value() || session_result->is_nil()) {
        spdlog::info("[ChatService] Whisper target {} is offline", target_user_id);
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateWhisperResponse(fbb,
            fbs::ChatMessageError_TARGET_OFFLINE, timestamp);
        fbb.Finish(resp);
        send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // Parse target session_id from Redis value
    auto target_session_id = static_cast<uint64_t>(
        std::stoull(session_result->str));

    // 2. Build WhisperMessage FBS and send to target via Kafka unicast
    {
        flatbuffers::FlatBufferBuilder fbb_msg(512);
        auto sender_name_off = fbb_msg.CreateString(std::to_string(sender_id));
        auto content_off = fbb_msg.CreateString(content_str);
        auto whisper_msg = fbs::CreateWhisperMessage(fbb_msg,
            sender_id, sender_name_off, content_off, timestamp);
        fbb_msg.Finish(whisper_msg);

        // Unicast to target session via Gateway
        send_response_with_flags(msg_ids::WHISPER_MESSAGE,
            envelope::routing_flags::DIRECTION_RESPONSE |
            envelope::routing_flags::DELIVERY_UNICAST,
            0 /* corr_id=0 for push */,
            0 /* core_id=0 — Gateway routes by session_id */,
            target_session_id,
            {fbb_msg.GetBufferPointer(), fbb_msg.GetSize()},
            meta.reply_topic);
    }

    // 3. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateWhisperResponse(fbb,
        fbs::ChatMessageError_NONE, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

// ============================================================
// Chat History (Task 6)
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_chat_history(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::ChatHistoryRequest>()) {
        spdlog::error("[ChatService] ChatHistoryRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateChatHistoryResponse(fbb,
            fbs::ChatMessageError_NOT_IN_ROOM);
        fbb.Finish(resp);
        send_response(msg_ids::CHAT_HISTORY_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<fbs::ChatHistoryRequest>(fbs_payload.data());
    auto room_id = req->room_id();
    auto before_message_id = req->before_message_id();
    auto limit = std::min(req->limit(), static_cast<uint32_t>(config_.history_page_size));
    auto user_id = meta.user_id;

    spdlog::info("[ChatService] handle_chat_history (room: {}, before: {}, limit: {}, corr_id: {})",
                 room_id, before_message_id, limit, meta.corr_id);

    auto core_id = apex::core::CoreEngine::current_core_id();

    // 1. SISMEMBER — membership check
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);
    auto is_member = co_await redis_data_.multiplexer(core_id).command(
        "SISMEMBER %s %s", members_key.c_str(), user_id_str.c_str());
    if (!is_member.has_value() || is_member->integer == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateChatHistoryResponse(fbb,
            fbs::ChatMessageError_NOT_IN_ROOM);
        fbb.Finish(resp);
        send_response(msg_ids::CHAT_HISTORY_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    // 2. PG query — cursor-based paging with LIMIT N+1 for has_more detection
    auto fetch_limit = limit + 1;  // fetch one extra to detect has_more

    apex::core::Result<apex::shared::adapters::pg::PgResult> msg_result =
        std::unexpected(apex::core::ErrorCode::AdapterError);
    if (before_message_id == 0) {
        // Latest messages
        std::array<std::string, 2> params = {
            std::to_string(room_id),
            std::to_string(fetch_limit)
        };
        msg_result = co_await pg_.query(
            "SELECT id, sender_id, sender_name, content, "
            "EXTRACT(EPOCH FROM created_at)::bigint * 1000 AS ts "
            "FROM chat_svc.chat_messages WHERE room_id = $1::bigint "
            "ORDER BY id DESC LIMIT $2::int",
            params);
    } else {
        // Before cursor
        std::array<std::string, 3> params = {
            std::to_string(room_id),
            std::to_string(before_message_id),
            std::to_string(fetch_limit)
        };
        msg_result = co_await pg_.query(
            "SELECT id, sender_id, sender_name, content, "
            "EXTRACT(EPOCH FROM created_at)::bigint * 1000 AS ts "
            "FROM chat_svc.chat_messages WHERE room_id = $1::bigint AND id < $2::bigint "
            "ORDER BY id DESC LIMIT $3::int",
            params);
    }

    if (!msg_result.has_value()) {
        spdlog::error("[ChatService] PG query chat_messages failed: {}",
                      apex::core::error_code_name(msg_result.error()));
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateChatHistoryResponse(fbb,
            fbs::ChatMessageError_NOT_IN_ROOM);
        fbb.Finish(resp);
        send_response(msg_ids::CHAT_HISTORY_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto& pg_res = *msg_result;
    bool has_more = (pg_res.row_count() > static_cast<int>(limit));
    auto actual_count = std::min(pg_res.row_count(), static_cast<int>(limit));

    // 3. Build HistoryMessage vector
    flatbuffers::FlatBufferBuilder fbb(1024);
    std::vector<flatbuffers::Offset<fbs::HistoryMessage>> msg_offsets;
    msg_offsets.reserve(static_cast<size_t>(actual_count));

    for (int i = 0; i < actual_count; ++i) {
        auto mid = static_cast<uint64_t>(
            std::stoull(std::string(pg_res.value(i, 0))));
        auto sid = static_cast<uint64_t>(
            std::stoull(std::string(pg_res.value(i, 1))));
        auto sname = pg_res.value(i, 2);
        auto mcontent = pg_res.value(i, 3);
        auto mts = static_cast<uint64_t>(
            std::stoull(std::string(pg_res.value(i, 4))));

        auto sname_off = fbb.CreateString(sname.data(), sname.size());
        auto content_off = fbb.CreateString(mcontent.data(), mcontent.size());
        msg_offsets.push_back(fbs::CreateHistoryMessage(fbb,
            mid, sid, sname_off, content_off, mts));
    }

    auto msgs_vec = fbb.CreateVector(msg_offsets);
    auto resp = fbs::CreateChatHistoryResponse(fbb,
        fbs::ChatMessageError_NONE,
        msgs_vec,
        has_more);
    fbb.Finish(resp);
    send_response(msg_ids::CHAT_HISTORY_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

// ============================================================
// Global Broadcast (Task 7)
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::handle_global_broadcast(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<fbs::GlobalBroadcastRequest>()) {
        spdlog::error("[ChatService] GlobalBroadcastRequest verification failed");
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateGlobalBroadcastResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<fbs::GlobalBroadcastRequest>(fbs_payload.data());
    auto content = req->content();

    // 1. Input validation
    if (!content || content->size() == 0) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto resp = fbs::CreateGlobalBroadcastResponse(fbb,
            fbs::ChatMessageError_EMPTY_MESSAGE);
        fbb.Finish(resp);
        send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
        co_return apex::core::ok();
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();
    auto sender_id = meta.user_id;

    spdlog::info("[ChatService] handle_global_broadcast (corr_id: {}, session: {})",
                 meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();

    // 1. Permission check — all authenticated users allowed for now (Wave 2 demo)
    //    Future: admin role check via Redis/PG

    // 2. Build GlobalChatMessage FBS
    auto global_channel = std::string("pub:global:chat");
    {
        flatbuffers::FlatBufferBuilder fbb_pub(512);
        auto sender_name_off = fbb_pub.CreateString(std::to_string(sender_id));
        auto content_off = fbb_pub.CreateString(content_str);
        auto channel_off = fbb_pub.CreateString(global_channel);
        auto global_msg = fbs::CreateGlobalChatMessage(fbb_pub,
            sender_id, sender_name_off, content_off, timestamp, channel_off);
        fbb_pub.Finish(global_msg);

        // 3. Redis PUBLISH to global channel
        auto pub_payload = build_pubsub_payload(msg_ids::GLOBAL_CHAT_MESSAGE,
            {fbb_pub.GetBufferPointer(), fbb_pub.GetSize()});
        auto pub_result = co_await redis_pubsub_.multiplexer(core_id).command(
            "PUBLISH %s %b", global_channel.c_str(),
            reinterpret_cast<const char*>(pub_payload.data()),
            static_cast<size_t>(pub_payload.size()));
        if (!pub_result.has_value()) {
            spdlog::warn("[ChatService] Redis PUBLISH failed for global broadcast");
        }
    }

    // 4. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateGlobalBroadcastResponse(fbb,
        fbs::ChatMessageError_NONE, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, meta.reply_topic);
    co_return apex::core::ok();
}

} // namespace apex::chat_svc
