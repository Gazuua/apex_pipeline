// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/chat_svc/chat_logic.hpp>
#include <apex/chat_svc/chat_service.hpp>

#include <apex/core/configure_context.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/protocols/kafka/envelope_builder.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

// FlatBuffers generated headers
#include <chat_message_generated.h>
#include <chat_room_generated.h>

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <string>

namespace apex::chat_svc
{

namespace envelope = apex::shared::protocols::kafka;

namespace
{

const apex::core::ScopedLogger& s_logger()
{
    static const apex::core::ScopedLogger instance{"ChatService", apex::core::ScopedLogger::NO_CORE, "app"};
    return instance;
}

/// Safe uint64 parsing from string_view. Returns Result<uint64_t> on failure (logs warning).
apex::core::Result<uint64_t> safe_parse_u64(std::string_view sv, std::string_view context = "") noexcept
{
    uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{})
    {
        s_logger().warn("Failed to parse uint64 '{}' (context: {})", sv, context);
        return std::unexpected(apex::core::ErrorCode::InvalidMessage);
    }
    return value;
}

struct SessionCore
{
    uint64_t session_id;
    uint16_t core_id;
};

/// Parse "session_id:core_id" from Redis value. Falls back to core_id=0 if no delimiter (legacy compat).
apex::core::Result<SessionCore> parse_session_core(std::string_view sv) noexcept
{
    auto colon = sv.find(':');
    if (colon == std::string_view::npos)
    {
        // Legacy format: session_id only → core_id=0 fallback
        auto sid = safe_parse_u64(sv, "parse_session_core.session_id");
        if (!sid.has_value())
            return std::unexpected(sid.error());
        return SessionCore{*sid, 0};
    }

    auto sid = safe_parse_u64(sv.substr(0, colon), "parse_session_core.session_id");
    if (!sid.has_value())
        return std::unexpected(sid.error());

    auto cid = safe_parse_u64(sv.substr(colon + 1), "parse_session_core.core_id");
    if (!cid.has_value())
        return std::unexpected(cid.error());

    return SessionCore{*sid, static_cast<uint16_t>(*cid)};
}

} // anonymous namespace

// ============================================================
// Construction / Lifecycle
// ============================================================

ChatService::ChatService(Config config)
    : ServiceBase("chat")
    , config_(std::move(config))
{}

void ChatService::on_configure(apex::core::ConfigureContext& ctx)
{
    // Phase 1: 어댑터 참조 취득
    kafka_ = &ctx.server.adapter<apex::shared::adapters::kafka::KafkaAdapter>();
    redis_data_ = &ctx.server.adapter<apex::shared::adapters::redis::RedisAdapter>("data");
    redis_pubsub_ = &ctx.server.adapter<apex::shared::adapters::redis::RedisAdapter>("pubsub");
    pg_ = &ctx.server.adapter<apex::shared::adapters::pg::PgAdapter>();

    logger_.info("on_configure: adapters acquired (core {})", ctx.core_id);
}

void ChatService::on_start()
{
    logger_.info("on_start: registering kafka_routes...");

    // 7개 kafka_route 등록 (+ 1개 global_broadcast = 8)
    kafka_route<fbs::CreateRoomRequest>(msg_ids::CREATE_ROOM_REQUEST, &ChatService::on_create_room);
    kafka_route<fbs::JoinRoomRequest>(msg_ids::JOIN_ROOM_REQUEST, &ChatService::on_join_room);
    kafka_route<fbs::LeaveRoomRequest>(msg_ids::LEAVE_ROOM_REQUEST, &ChatService::on_leave_room);
    kafka_route<fbs::ListRoomsRequest>(msg_ids::LIST_ROOMS_REQUEST, &ChatService::on_list_rooms);
    kafka_route<fbs::SendMessageRequest>(msg_ids::SEND_MESSAGE_REQUEST, &ChatService::on_send_message);
    kafka_route<fbs::WhisperRequest>(msg_ids::WHISPER_REQUEST, &ChatService::on_whisper);
    kafka_route<fbs::ChatHistoryRequest>(msg_ids::CHAT_HISTORY_REQUEST, &ChatService::on_chat_history);
    kafka_route<fbs::GlobalBroadcastRequest>(msg_ids::GLOBAL_BROADCAST_REQUEST, &ChatService::on_global_broadcast);

    // PG connection warm-up — spawn()으로 tracked 코루틴 실행
    if (pg_)
    {
        spawn([this]() -> boost::asio::awaitable<void> {
            auto result = co_await pg_->query("SELECT 1");
            if (result.has_value())
            {
                logger_.info("PG connection warm-up OK");
            }
            else
            {
                logger_.warn("PG warm-up failed (will retry on first query)");
            }
        });
    }

    logger_.info("Started. 8 kafka_routes registered. Consuming from: {}", config_.request_topic);
}

void ChatService::on_stop()
{
    logger_.info("on_stop — cleaning up");
    kafka_ = nullptr;
    redis_data_ = nullptr;
    redis_pubsub_ = nullptr;
    pg_ = nullptr;
}

// ============================================================
// Helpers
// ============================================================

uint64_t ChatService::current_timestamp_ms() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void ChatService::send_response(uint32_t msg_id, uint64_t corr_id, uint16_t core_id, apex::core::SessionId session_id,
                                std::span<const uint8_t> fbs_payload, const std::string& reply_topic)
{
    send_response_with_flags(msg_id, envelope::routing_flags::DIRECTION_RESPONSE, corr_id, core_id, session_id,
                             fbs_payload, reply_topic);
}

void ChatService::send_response_with_flags(uint32_t msg_id, uint16_t flags, uint64_t corr_id, uint16_t core_id,
                                           apex::core::SessionId session_id, std::span<const uint8_t> fbs_payload,
                                           const std::string& reply_topic)
{
    auto envelope_buf =
        envelope::EnvelopeBuilder{}
            .routing(msg_id, flags)
            .metadata(core_id, corr_id, envelope::source_ids::CHAT, apex::core::to_underlying(session_id), 0)
            .payload(fbs_payload)
            .build();

    // Reply-To: reply_topic이 있으면 그쪽으로 응답, 없으면 fallback
    const auto& target_topic = reply_topic.empty() ? config_.response_topic : reply_topic;

    // Use session_id as Kafka key (design doc section 7.1)
    auto key = std::to_string(apex::core::to_underlying(session_id));
    logger_.trace("kafka produce topic={} msg_id={} size={}", target_topic, msg_id, envelope_buf.size());
    auto result = kafka_->produce(target_topic, key, std::span<const uint8_t>(envelope_buf));

    if (!result.has_value())
    {
        logger_.error("Failed to produce response to '{}' (msg_id: {}, corr_id: {})", target_topic, msg_id, corr_id);
    }
}

std::vector<uint8_t> ChatService::build_pubsub_payload(uint32_t msg_id, std::span<const uint8_t> fbs_payload) const
{
    // Format: [msg_id(u32 BE)] + [fbs payload]
    // Gateway BroadcastFanout reads msg_id as big-endian to build WireHeader.
    // NOTE: std::array + insert avoids GCC 14 -Wfree-nonheap-object false positive
    //       that occurs with reserve() + repeated push_back() inlining.
    const std::array<uint8_t, 4> header = {
        static_cast<uint8_t>((msg_id >> 24) & 0xFF),
        static_cast<uint8_t>((msg_id >> 16) & 0xFF),
        static_cast<uint8_t>((msg_id >> 8) & 0xFF),
        static_cast<uint8_t>(msg_id & 0xFF),
    };

    std::vector<uint8_t> buf;
    buf.reserve(header.size() + fbs_payload.size());
    buf.insert(buf.end(), header.begin(), header.end());
    buf.insert(buf.end(), fbs_payload.begin(), fbs_payload.end());
    return buf;
}

// ============================================================
// Room Management Handlers
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::on_create_room(const apex::core::KafkaMessageMeta& meta,
                                                                             uint32_t /*msg_id*/,
                                                                             const fbs::CreateRoomRequest* req)
{
    // 1. Input validation
    auto room_name = req->room_name();
    auto name_validation = validate_room_name(room_name ? room_name->size() : 0, config_.max_room_name_length);
    if (name_validation == RoomNameValidation::EMPTY)
    {
        co_return co_await send_create_room_error(meta, fbs::ChatRoomError_ROOM_NAME_EMPTY);
    }
    if (name_validation == RoomNameValidation::TOO_LONG)
    {
        co_return co_await send_create_room_error(meta, fbs::ChatRoomError_ROOM_NAME_TOO_LONG);
    }

    auto room_name_str = std::string(room_name->string_view());
    auto max_members = req->max_members() > 0 ? req->max_members() : config_.max_room_members;
    auto user_id = meta.user_id;

    logger_.info("on_create_room (name: {}, max: {}, corr_id: {}, session: {})", room_name_str, max_members,
                 meta.corr_id, meta.session_id);

    // 2. PG INSERT -- create room record
    std::array<std::string, 3> insert_params = {room_name_str, std::to_string(max_members), std::to_string(user_id)};
    auto pg_result = co_await pg_->query("INSERT INTO chat_svc.chat_rooms (room_name, max_members, owner_id) "
                                         "VALUES ($1, $2::int, $3::bigint) RETURNING room_id",
                                         insert_params);

    if (!pg_result.has_value() || pg_result->row_count() == 0)
    {
        logger_.error("PG INSERT chat_rooms failed: {}",
                      pg_result.has_value() ? "no rows returned" : apex::core::error_code_name(pg_result.error()));
        co_return co_await send_create_room_error(meta, fbs::ChatRoomError_INTERNAL_ERROR);
    }

    auto room_id_result = safe_parse_u64(pg_result->value(0, 0), "create_room.room_id");
    if (!room_id_result.has_value())
    {
        logger_.error("PG returned invalid room_id for create_room");
        co_return co_await send_create_room_error(meta, fbs::ChatRoomError_INTERNAL_ERROR);
    }
    auto room_id = *room_id_result;

    // 3. Redis SADD -- add owner as first member
    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);
    auto redis_result =
        co_await redis_data_->multiplexer(core_id).command("SADD %s %s", members_key.c_str(), user_id_str.c_str());
    if (!redis_result.has_value())
    {
        logger_.warn("Redis SADD members failed for room {}", room_id);
        // Non-fatal: room created in PG, membership will be eventually consistent
    }

    // 4. Send CreateRoomResponse with actual room_id
    logger_.debug("room created id={} name={} owner={}", room_id, room_name_str, user_id);
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name_off = fbb.CreateString(room_name_str);
    auto resp = fbs::CreateCreateRoomResponse(fbb, fbs::ChatRoomError_NONE, room_id, name_off);
    fbb.Finish(resp);
    send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> ChatService::on_join_room(const apex::core::KafkaMessageMeta& meta,
                                                                           uint32_t /*msg_id*/,
                                                                           const fbs::JoinRoomRequest* req)
{
    auto room_id = req->room_id();
    auto user_id = meta.user_id;

    logger_.info("on_join_room (room: {}, corr_id: {}, session: {})", room_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);

    // 1. Check room existence in PG
    std::array<std::string, 1> room_params = {std::to_string(room_id)};
    auto room_result = co_await pg_->query("SELECT room_name, max_members FROM chat_svc.chat_rooms "
                                           "WHERE room_id = $1::bigint AND is_active = true",
                                           room_params);

    if (!room_result.has_value() || room_result->row_count() == 0)
    {
        logger_.warn("join_room: room {} not found", room_id);
        co_return co_await send_join_room_error(meta, fbs::ChatRoomError_ROOM_NOT_FOUND, room_id);
    }

    auto room_name_sv = room_result->value(0, 0);
    auto max_members_result = safe_parse_u64(room_result->value(0, 1), "join_room.max_members");
    if (!max_members_result.has_value())
        co_return std::unexpected(max_members_result.error());
    auto max_members = static_cast<uint32_t>(*max_members_result);

    // 2. Atomic join: SISMEMBER + SCARD check + SADD in a single Lua script.
    // Eliminates TOCTOU race between SCARD and SADD that could exceed max_members.
    // Returns: -1 = already in room, 0 = room full, >0 = new member count (success).
    static constexpr std::string_view JOIN_ROOM_LUA = R"lua(
if redis.call('SISMEMBER', KEYS[1], ARGV[1]) == 1 then
    return -1
end
if redis.call('SCARD', KEYS[1]) >= tonumber(ARGV[2]) then
    return 0
end
redis.call('SADD', KEYS[1], ARGV[1])
return redis.call('SCARD', KEYS[1])
)lua";

    auto max_members_str = std::to_string(max_members);
    auto lua_script = std::string(JOIN_ROOM_LUA);
    auto eval_result = co_await redis_data_->multiplexer(core_id).command(
        "EVAL %s 1 %s %s %s", lua_script.c_str(), members_key.c_str(), user_id_str.c_str(), max_members_str.c_str());

    if (!eval_result.has_value() || eval_result->is_error() || !eval_result->is_integer())
    {
        logger_.warn("Redis EVAL failed for join_room (room: {})", room_id);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto lua_result = eval_result->integer;
    logger_.trace("on_join_room: Lua EVAL result={} (room={}, user={})", lua_result, room_id, user_id);
    auto join_result = interpret_join_result(lua_result);
    if (join_result == JoinRoomResult::ALREADY_IN)
    {
        co_return co_await send_join_room_error(meta, fbs::ChatRoomError_ALREADY_IN_ROOM, room_id);
    }
    if (join_result == JoinRoomResult::ROOM_FULL)
    {
        co_return co_await send_join_room_error(meta, fbs::ChatRoomError_ROOM_FULL, room_id);
    }

    auto member_count = static_cast<uint32_t>(lua_result);
    logger_.debug("join_room ok room={} user={} members={}", room_id, user_id, member_count);

    // 6. Send JoinRoomResponse
    flatbuffers::FlatBufferBuilder fbb(256);
    auto name_off = fbb.CreateString(room_name_sv.data(), room_name_sv.size());
    auto resp = fbs::CreateJoinRoomResponse(fbb, fbs::ChatRoomError_NONE, room_id, name_off, member_count);
    fbb.Finish(resp);
    send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> ChatService::on_leave_room(const apex::core::KafkaMessageMeta& meta,
                                                                            uint32_t /*msg_id*/,
                                                                            const fbs::LeaveRoomRequest* req)
{
    auto room_id = req->room_id();
    auto user_id = meta.user_id;

    logger_.info("on_leave_room (room: {}, corr_id: {}, session: {})", room_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);

    // 1. Atomic leave: SISMEMBER + SREM in a single Lua script.
    // Eliminates TOCTOU race between SISMEMBER and SREM on concurrent leave requests.
    // Returns: 0 = not in room, 1 = successfully removed.
    static constexpr std::string_view LEAVE_ROOM_LUA = R"lua(
if redis.call('SISMEMBER', KEYS[1], ARGV[1]) == 0 then
    return 0
end
redis.call('SREM', KEYS[1], ARGV[1])
return 1
)lua";

    auto lua_script = std::string(LEAVE_ROOM_LUA);
    auto eval_result = co_await redis_data_->multiplexer(core_id).command("EVAL %s 1 %s %s", lua_script.c_str(),
                                                                          members_key.c_str(), user_id_str.c_str());

    if (!eval_result.has_value() || eval_result->is_error() || !eval_result->is_integer())
    {
        logger_.warn("Redis EVAL failed for leave_room (room: {})", room_id);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    if (eval_result->integer == 0)
    {
        co_return co_await send_leave_room_error(meta, fbs::ChatRoomError_NOT_IN_ROOM, room_id);
    }
    logger_.debug("leave_room ok room={} user={}", room_id, user_id);

    // 3. Send LeaveRoomResponse
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateLeaveRoomResponse(fbb, fbs::ChatRoomError_NONE, room_id);
    fbb.Finish(resp);
    send_response(msg_ids::LEAVE_ROOM_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> ChatService::on_list_rooms(const apex::core::KafkaMessageMeta& meta,
                                                                            uint32_t /*msg_id*/,
                                                                            const fbs::ListRoomsRequest* req)
{
    auto offset = req->offset();
    auto limit = std::min(req->limit(), config_.max_list_rooms_limit);

    logger_.info("on_list_rooms (offset: {}, limit: {}, corr_id: {})", offset, limit, meta.corr_id);

    // 1. PG: Get total count
    auto count_result = co_await pg_->query("SELECT COUNT(*) FROM chat_svc.chat_rooms WHERE is_active = true");

    uint32_t total_count = 0;
    if (count_result.has_value() && count_result->row_count() > 0)
    {
        auto total_count_result = safe_parse_u64(count_result->value(0, 0), "list_rooms.total_count");
        if (!total_count_result.has_value())
            co_return std::unexpected(total_count_result.error());
        total_count = static_cast<uint32_t>(*total_count_result);
    }

    // 2. PG: Get rooms page
    std::array<std::string, 2> page_params = {std::to_string(limit), std::to_string(offset)};
    auto rooms_result = co_await pg_->query("SELECT room_id, room_name, max_members, owner_id "
                                            "FROM chat_svc.chat_rooms WHERE is_active = true "
                                            "ORDER BY room_id ASC LIMIT $1::int OFFSET $2::int",
                                            page_params);

    if (!rooms_result.has_value())
    {
        logger_.error("PG query chat_rooms failed: {}", apex::core::error_code_name(rooms_result.error()));
        co_return co_await send_list_rooms_error(meta, fbs::ChatRoomError_INTERNAL_ERROR);
    }

    // 3. Build RoomInfo vector with real-time member count from Redis
    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& pg_res = *rooms_result;

    flatbuffers::FlatBufferBuilder fbb(512);
    std::vector<flatbuffers::Offset<fbs::RoomInfo>> room_offsets;
    room_offsets.reserve(static_cast<size_t>(pg_res.row_count()));

    for (int i = 0; i < pg_res.row_count(); ++i)
    {
        auto rid_result = safe_parse_u64(pg_res.value(i, 0), "list_rooms.room_id");
        if (!rid_result.has_value())
            continue;
        auto rid = *rid_result;
        auto rname = pg_res.value(i, 1);
        auto rmax_result = safe_parse_u64(pg_res.value(i, 2), "list_rooms.max_members");
        if (!rmax_result.has_value())
            continue;
        auto rmax = static_cast<uint32_t>(*rmax_result);
        auto rowner_result = safe_parse_u64(pg_res.value(i, 3), "list_rooms.owner_id");
        if (!rowner_result.has_value())
            continue;
        auto rowner = *rowner_result;

        // Redis SCARD for real-time member count
        auto members_key = std::format("chat:room:{}:members", rid);
        uint32_t member_count = 0;
        auto scard = co_await redis_data_->multiplexer(core_id).command("SCARD %s", members_key.c_str());
        if (scard.has_value())
        {
            member_count = static_cast<uint32_t>(scard->integer);
        }

        auto name_off = fbb.CreateString(rname.data(), rname.size());
        room_offsets.push_back(fbs::CreateRoomInfo(fbb, rid, name_off, member_count, rmax, rowner));
    }

    auto rooms_vec = fbb.CreateVector(room_offsets);
    auto resp = fbs::CreateListRoomsResponse(fbb, fbs::ChatRoomError_NONE, rooms_vec, total_count);
    fbb.Finish(resp);
    send_response(msg_ids::LIST_ROOMS_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

// ============================================================
// Message Send + Redis Pub/Sub Broadcast
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::on_send_message(const apex::core::KafkaMessageMeta& meta,
                                                                              uint32_t /*msg_id*/,
                                                                              const fbs::SendMessageRequest* req)
{
    auto room_id = req->room_id();
    auto content = req->content();

    // 1. Input validation
    auto msg_validation = validate_message_content(content ? content->size() : 0, config_.max_message_length);
    if (msg_validation == MessageValidation::EMPTY)
    {
        co_return co_await send_message_error(meta, fbs::ChatMessageError_EMPTY_MESSAGE);
    }
    if (msg_validation == MessageValidation::TOO_LONG)
    {
        co_return co_await send_message_error(meta, fbs::ChatMessageError_MESSAGE_TOO_LONG);
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();
    auto user_id = meta.user_id;

    logger_.info("on_send_message (room: {}, corr_id: {}, session: {})", room_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);

    // 2. SISMEMBER -- membership check
    auto is_member =
        co_await redis_data_->multiplexer(core_id).command("SISMEMBER %s %s", members_key.c_str(), user_id_str.c_str());
    if (!is_member.has_value())
    {
        logger_.warn("Redis SISMEMBER failed for send_message (room: {})", room_id);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    if (is_member->integer == 0)
    {
        co_return co_await send_message_error(meta, fbs::ChatMessageError_NOT_IN_ROOM);
    }

    // 3. Redis PUBLISH -- real-time broadcast via pub/sub
    auto channel = std::format("pub:chat:room:{}", room_id);
    {
        flatbuffers::FlatBufferBuilder fbb_pub(512);
        auto sender_off = fbb_pub.CreateString(user_id_str);
        auto content_off = fbb_pub.CreateString(content_str);
        auto channel_off = fbb_pub.CreateString(channel);
        auto chat_msg = fbs::CreateChatMessage(fbb_pub, room_id, user_id, sender_off, content_off,
                                               0 /* message_id assigned by DB */, timestamp, channel_off);
        fbb_pub.Finish(chat_msg);

        auto pub_payload = build_pubsub_payload(msg_ids::CHAT_MESSAGE, {fbb_pub.GetBufferPointer(), fbb_pub.GetSize()});
        logger_.trace("on_send_message: Redis PUBLISH (channel={}, payload_size={})", channel, pub_payload.size());
        auto pub_result = co_await redis_pubsub_->multiplexer(core_id).command(
            "PUBLISH %s %b", channel.c_str(), reinterpret_cast<const char*>(pub_payload.data()),
            static_cast<size_t>(pub_payload.size()));
        if (!pub_result.has_value())
        {
            logger_.warn("Redis PUBLISH failed for room {}", room_id);
        }
    }

    // 4. Kafka produce to persist topic for async DB storage
    {
        flatbuffers::FlatBufferBuilder fbb_db(512);
        auto sender_off = fbb_db.CreateString(user_id_str);
        auto content_off = fbb_db.CreateString(content_str);
        auto db_msg = fbs::CreateChatMessage(fbb_db, room_id, user_id, sender_off, content_off, 0, timestamp, 0);
        fbb_db.Finish(db_msg);

        logger_.trace("on_send_message: Kafka produce (topic={}, payload_size={})", config_.persist_topic,
                      fbb_db.GetSize());
        auto produce_result = kafka_->produce(config_.persist_topic, std::to_string(room_id),
                                              std::span<const uint8_t>(fbb_db.GetBufferPointer(), fbb_db.GetSize()));
        if (!produce_result.has_value())
        {
            logger_.error("Kafka persist produce failed for room {}: {}", room_id,
                          apex::core::error_code_name(produce_result.error()));
        }
    }

    // 5. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateSendMessageResponse(fbb, fbs::ChatMessageError_NONE, 0, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

// ============================================================
// Whisper
// ============================================================

boost::asio::awaitable<apex::core::Result<void>>
ChatService::on_whisper(const apex::core::KafkaMessageMeta& meta, uint32_t /*msg_id*/, const fbs::WhisperRequest* req)
{
    auto target_user_id = req->target_user_id();
    auto content = req->content();

    // 1. Input validation
    auto whisper_validation = validate_message_content(content ? content->size() : 0, config_.max_message_length);
    if (whisper_validation == MessageValidation::EMPTY)
    {
        co_return co_await send_whisper_error(meta, fbs::ChatMessageError_EMPTY_MESSAGE);
    }
    if (whisper_validation == MessageValidation::TOO_LONG)
    {
        co_return co_await send_whisper_error(meta, fbs::ChatMessageError_MESSAGE_TOO_LONG);
    }

    if (target_user_id == meta.user_id)
    {
        logger_.warn("Whisper to self rejected (user_id: {})", meta.user_id);
        co_return co_await send_whisper_error(meta, fbs::ChatMessageError_PERMISSION_DENIED);
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();
    auto sender_id = meta.user_id;

    logger_.info("on_whisper (target: {}, corr_id: {}, session: {})", target_user_id, meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();

    // 2. Redis GET -- lookup target user's session_id for online check
    auto session_key = std::format("session:user:{}", target_user_id);
    auto session_result = co_await redis_data_->multiplexer(core_id).command("GET %s", session_key.c_str());

    if (!session_result.has_value() || session_result->is_nil())
    {
        logger_.info("Whisper target {} is offline", target_user_id);
        co_return co_await send_whisper_error(meta, fbs::ChatMessageError_TARGET_OFFLINE);
    }

    // Parse "session_id:core_id" from Redis value (O(1) core routing)
    auto target_result = parse_session_core(session_result->str);
    if (!target_result.has_value())
        co_return std::unexpected(target_result.error());
    auto target_session_id = apex::core::make_session_id(target_result->session_id);
    auto target_core_id = target_result->core_id;

    // 3. Build WhisperMessage FBS and send to target via Kafka unicast
    {
        flatbuffers::FlatBufferBuilder fbb_msg(512);
        auto sender_name_off = fbb_msg.CreateString(std::to_string(sender_id));
        auto content_off = fbb_msg.CreateString(content_str);
        auto whisper_msg = fbs::CreateWhisperMessage(fbb_msg, sender_id, sender_name_off, content_off, timestamp);
        fbb_msg.Finish(whisper_msg);

        // Unicast to target session via Gateway (O(1) — target core_id from Redis)
        send_response_with_flags(msg_ids::WHISPER_MESSAGE,
                                 envelope::routing_flags::DIRECTION_RESPONSE |
                                     envelope::routing_flags::DELIVERY_UNICAST,
                                 0 /* corr_id=0 for push */, target_core_id, target_session_id,
                                 {fbb_msg.GetBufferPointer(), fbb_msg.GetSize()}, "");
    }

    // 4. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateWhisperResponse(fbb, fbs::ChatMessageError_NONE, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

// ============================================================
// Chat History
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> ChatService::on_chat_history(const apex::core::KafkaMessageMeta& meta,
                                                                              uint32_t /*msg_id*/,
                                                                              const fbs::ChatHistoryRequest* req)
{
    auto room_id = req->room_id();
    auto before_message_id = req->before_message_id();
    auto limit = std::min(req->limit(), static_cast<uint32_t>(config_.history_page_size));
    auto user_id = meta.user_id;

    logger_.info("on_chat_history (room: {}, before: {}, limit: {}, corr_id: {})", room_id, before_message_id, limit,
                 meta.corr_id);

    auto core_id = apex::core::CoreEngine::current_core_id();

    // 1. SISMEMBER -- membership check
    auto members_key = std::format("chat:room:{}:members", room_id);
    auto user_id_str = std::to_string(user_id);
    auto is_member =
        co_await redis_data_->multiplexer(core_id).command("SISMEMBER %s %s", members_key.c_str(), user_id_str.c_str());
    if (!is_member.has_value())
    {
        logger_.warn("Redis SISMEMBER failed for chat_history (room: {})", room_id);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    if (is_member->integer == 0)
    {
        co_return co_await send_history_error(meta, fbs::ChatMessageError_NOT_IN_ROOM);
    }

    // 2. PG query -- cursor-based paging with LIMIT N+1 for has_more detection
    auto fetch_limit = limit + 1; // fetch one extra to detect has_more

    apex::core::Result<apex::shared::adapters::pg::PgResult> msg_result =
        std::unexpected(apex::core::ErrorCode::AdapterError);
    if (before_message_id == 0)
    {
        // Latest messages
        std::array<std::string, 2> params = {std::to_string(room_id), std::to_string(fetch_limit)};
        msg_result = co_await pg_->query("SELECT message_id, sender_id, sender_name, content, "
                                         "EXTRACT(EPOCH FROM created_at)::bigint * 1000 AS ts "
                                         "FROM chat_svc.chat_messages WHERE room_id = $1::bigint "
                                         "ORDER BY message_id DESC LIMIT $2::int",
                                         params);
    }
    else
    {
        // Before cursor
        std::array<std::string, 3> params = {std::to_string(room_id), std::to_string(before_message_id),
                                             std::to_string(fetch_limit)};
        msg_result =
            co_await pg_->query("SELECT message_id, sender_id, sender_name, content, "
                                "EXTRACT(EPOCH FROM created_at)::bigint * 1000 AS ts "
                                "FROM chat_svc.chat_messages WHERE room_id = $1::bigint AND message_id < $2::bigint "
                                "ORDER BY message_id DESC LIMIT $3::int",
                                params);
    }

    if (!msg_result.has_value())
    {
        logger_.error("PG query chat_messages failed: {}", apex::core::error_code_name(msg_result.error()));
        co_return co_await send_history_error(meta, fbs::ChatMessageError_INTERNAL_ERROR);
    }

    auto& pg_res = *msg_result;
    bool has_more = (pg_res.row_count() > static_cast<int>(limit));
    auto actual_count = std::min(pg_res.row_count(), static_cast<int>(limit));

    // 3. Build HistoryMessage vector
    flatbuffers::FlatBufferBuilder fbb(1024);
    std::vector<flatbuffers::Offset<fbs::HistoryMessage>> msg_offsets;
    msg_offsets.reserve(static_cast<size_t>(actual_count));

    for (int i = 0; i < actual_count; ++i)
    {
        auto mid_result = safe_parse_u64(pg_res.value(i, 0), "history.message_id");
        if (!mid_result.has_value())
            continue;
        auto mid = *mid_result;
        auto sid_result = safe_parse_u64(pg_res.value(i, 1), "history.sender_id");
        if (!sid_result.has_value())
            continue;
        auto sid = *sid_result;
        auto sname = pg_res.value(i, 2);
        auto mcontent = pg_res.value(i, 3);
        auto mts_result = safe_parse_u64(pg_res.value(i, 4), "history.timestamp");
        if (!mts_result.has_value())
            continue;
        auto mts = *mts_result;

        auto sname_off = fbb.CreateString(sname.data(), sname.size());
        auto content_off = fbb.CreateString(mcontent.data(), mcontent.size());
        msg_offsets.push_back(fbs::CreateHistoryMessage(fbb, mid, sid, sname_off, content_off, mts));
    }

    auto msgs_vec = fbb.CreateVector(msg_offsets);
    auto resp = fbs::CreateChatHistoryResponse(fbb, fbs::ChatMessageError_NONE, msgs_vec, has_more);
    fbb.Finish(resp);
    send_response(msg_ids::CHAT_HISTORY_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

// ============================================================
// Global Broadcast
// ============================================================

boost::asio::awaitable<apex::core::Result<void>>
ChatService::on_global_broadcast(const apex::core::KafkaMessageMeta& meta, uint32_t /*msg_id*/,
                                 const fbs::GlobalBroadcastRequest* req)
{
    auto content = req->content();

    // 1. Input validation
    auto msg_validation = validate_message_content(content ? content->size() : 0, config_.max_message_length);
    if (msg_validation == MessageValidation::EMPTY)
    {
        co_return co_await send_global_broadcast_error(meta, fbs::ChatMessageError_EMPTY_MESSAGE);
    }
    if (msg_validation == MessageValidation::TOO_LONG)
    {
        co_return co_await send_global_broadcast_error(meta, fbs::ChatMessageError_MESSAGE_TOO_LONG);
    }

    auto content_str = std::string(content->string_view());
    auto timestamp = current_timestamp_ms();
    auto sender_id = meta.user_id;

    logger_.info("on_global_broadcast (corr_id: {}, session: {})", meta.corr_id, meta.session_id);

    auto core_id = apex::core::CoreEngine::current_core_id();

    // 2. Build GlobalChatMessage FBS
    auto global_channel = std::string(GLOBAL_CHAT_CHANNEL);
    {
        flatbuffers::FlatBufferBuilder fbb_pub(512);
        auto sender_name_off = fbb_pub.CreateString(std::to_string(sender_id));
        auto content_off = fbb_pub.CreateString(content_str);
        auto channel_off = fbb_pub.CreateString(global_channel);
        auto global_msg =
            fbs::CreateGlobalChatMessage(fbb_pub, sender_id, sender_name_off, content_off, timestamp, channel_off);
        fbb_pub.Finish(global_msg);

        // 3. Redis PUBLISH to global channel
        auto pub_payload =
            build_pubsub_payload(msg_ids::GLOBAL_CHAT_MESSAGE, {fbb_pub.GetBufferPointer(), fbb_pub.GetSize()});
        logger_.trace("on_global_broadcast: Redis PUBLISH (channel={}, payload_size={})", global_channel,
                      pub_payload.size());
        auto pub_result = co_await redis_pubsub_->multiplexer(core_id).command(
            "PUBLISH %s %b", global_channel.c_str(), reinterpret_cast<const char*>(pub_payload.data()),
            static_cast<size_t>(pub_payload.size()));
        if (!pub_result.has_value())
        {
            logger_.warn("Redis PUBLISH failed for global broadcast");
        }
    }

    // 4. Sender confirmation
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateGlobalBroadcastResponse(fbb, fbs::ChatMessageError_NONE, timestamp);
    fbb.Finish(resp);
    send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

// ============================================================
// Error Response Helpers [D5]
// ============================================================

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_create_room_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateCreateRoomResponse(fbb, static_cast<fbs::ChatRoomError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::CREATE_ROOM_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_join_room_error(const apex::core::KafkaMessageMeta& meta, uint16_t error, uint64_t room_id)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateJoinRoomResponse(fbb, static_cast<fbs::ChatRoomError>(error), room_id);
    fbb.Finish(resp);
    send_response(msg_ids::JOIN_ROOM_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_leave_room_error(const apex::core::KafkaMessageMeta& meta, uint16_t error, uint64_t room_id)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateLeaveRoomResponse(fbb, static_cast<fbs::ChatRoomError>(error), room_id);
    fbb.Finish(resp);
    send_response(msg_ids::LEAVE_ROOM_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_list_rooms_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateListRoomsResponse(fbb, static_cast<fbs::ChatRoomError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::LIST_ROOMS_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_message_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateSendMessageResponse(fbb, static_cast<fbs::ChatMessageError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::SEND_MESSAGE_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_whisper_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateWhisperResponse(fbb, static_cast<fbs::ChatMessageError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::WHISPER_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_history_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateChatHistoryResponse(fbb, static_cast<fbs::ChatMessageError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::CHAT_HISTORY_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
ChatService::send_global_broadcast_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = fbs::CreateGlobalBroadcastResponse(fbb, static_cast<fbs::ChatMessageError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::GLOBAL_BROADCAST_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, "");
    co_return apex::core::ok();
}

} // namespace apex::chat_svc
