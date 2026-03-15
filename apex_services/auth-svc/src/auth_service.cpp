#include <apex/auth_svc/auth_service.hpp>
#include <apex/auth_svc/crypto_util.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

// Generated FlatBuffers headers
#include <login_request_generated.h>
#include <login_response_generated.h>
#include <logout_request_generated.h>
#include <logout_response_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <format>

namespace apex::auth_svc {

namespace envelope = apex::shared::protocols::kafka;

// msg_id constants (from msg_registry.toml)
namespace msg_ids {
    constexpr uint32_t LOGIN_REQUEST = 1000;
    constexpr uint32_t LOGIN_RESPONSE = 1001;
    constexpr uint32_t LOGOUT_REQUEST = 1002;
    constexpr uint32_t LOGOUT_RESPONSE = 1003;
    constexpr uint32_t REFRESH_TOKEN_REQUEST = 10;
    constexpr uint32_t REFRESH_TOKEN_RESPONSE = 11;
} // namespace msg_ids

AuthService::AuthService(
    AuthConfig config,
    apex::shared::adapters::kafka::KafkaAdapter& kafka,
    apex::shared::adapters::redis::RedisAdapter& redis,
    apex::shared::adapters::pg::PgAdapter& pg)
    : config_(std::move(config))
    , kafka_(kafka)
    , redis_(redis)
    , pg_(pg)
    , jwt_manager_(config_.jwt_private_key_path,
                   config_.jwt_public_key_path,
                   config_.jwt_issuer,
                   config_.access_token_ttl)
    , password_hasher_(config_.bcrypt_work_factor)
    , session_store_(redis_,
                     config_.redis_session_prefix,
                     config_.redis_blacklist_prefix,
                     config_.session_ttl)
{}

AuthService::~AuthService() {
    if (started_) {
        stop();
    }
}

void AuthService::start() {
    spdlog::info("[AuthService] Starting...");

    // Register Kafka consumer callback
    kafka_.set_message_callback(
        [this](std::string_view topic, int32_t partition,
               std::span<const uint8_t> key,
               std::span<const uint8_t> payload,
               int64_t offset) -> apex::core::Result<void> {
            return on_kafka_message(topic, partition, key, payload, offset);
        });

    started_ = true;
    spdlog::info("[AuthService] Started. Consuming from: {}", config_.request_topic);
}

void AuthService::stop() {
    spdlog::info("[AuthService] Stopping...");
    started_ = false;
}

apex::core::Result<void> AuthService::on_kafka_message(
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

void AuthService::dispatch_envelope(std::span<const uint8_t> payload) {
    if (payload.size() < envelope::ENVELOPE_HEADER_SIZE) {
        spdlog::error("[AuthService] Envelope too small: {} bytes", payload.size());
        return;
    }

    // Parse RoutingHeader (8B)
    auto routing_result = envelope::RoutingHeader::parse(payload);
    if (!routing_result.has_value()) {
        spdlog::error("[AuthService] Failed to parse RoutingHeader");
        return;
    }

    // Parse MetadataPrefix (32B) starting at offset 8
    auto meta_result = envelope::MetadataPrefix::parse(
        payload.subspan(envelope::RoutingHeader::SIZE));
    if (!meta_result.has_value()) {
        spdlog::error("[AuthService] Failed to parse MetadataPrefix");
        return;
    }

    auto& routing = *routing_result;
    auto& metadata = *meta_result;

    // FlatBuffers payload starts at offset 40
    auto fbs_payload = payload.subspan(envelope::ENVELOPE_HEADER_SIZE);

    switch (routing.msg_id) {
    case msg_ids::LOGIN_REQUEST:
        handle_login(fbs_payload, metadata.corr_id,
                     metadata.core_id, metadata.session_id);
        break;
    case msg_ids::LOGOUT_REQUEST:
        handle_logout(fbs_payload, metadata.corr_id,
                      metadata.core_id, metadata.session_id);
        break;
    case msg_ids::REFRESH_TOKEN_REQUEST:
        handle_refresh_token(fbs_payload, metadata.corr_id,
                             metadata.core_id, metadata.session_id);
        break;
    default:
        spdlog::warn("[AuthService] Unknown msg_id: {}", routing.msg_id);
        break;
    }
}

void AuthService::handle_login(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id)
{
    // 1. FlatBuffers verify + parse
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<apex::auth_svc::schemas::LoginRequest>()) {
        spdlog::error("[AuthService] LoginRequest verification failed");
        // Send error response
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder,
            apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, corr_id, core_id, session_id,
                      {builder.GetBufferPointer(), builder.GetSize()});
        return;
    }

    auto* req = flatbuffers::GetRoot<apex::auth_svc::schemas::LoginRequest>(
        fbs_payload.data());

    if (!req->email() || !req->password()) {
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder,
            apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, corr_id, core_id, session_id,
                      {builder.GetBufferPointer(), builder.GetSize()});
        return;
    }

    auto email = std::string(req->email()->string_view());
    auto password = std::string(req->password()->string_view());

    // NOTE: Full login flow requires async coroutine for PG/Redis calls.
    // Kafka consumer callback is synchronous; in production, co_spawn a coroutine.
    // Current implementation is synchronous placeholder -- async integration
    // will be completed in E2E phase (Plan 5).

    spdlog::info("[AuthService] handle_login (email: {}, corr_id: {}, session: {})",
                 email, corr_id, session_id);

    // Placeholder: send success response with empty tokens
    // Real implementation will:
    // 1. PG query for user by email
    // 2. Check account lock status
    // 3. Verify password with bcrypt
    // 4. Issue JWT access_token + refresh_token
    // 5. Store refresh_token hash in PG
    // 6. Create Redis session
    // 7. Send LoginResponse

    flatbuffers::FlatBufferBuilder builder(256);
    auto resp = apex::auth_svc::schemas::CreateLoginResponse(
        builder,
        apex::auth_svc::schemas::LoginError_NONE,
        0,   // access_token (placeholder)
        0,   // refresh_token (placeholder)
        0,   // user_id (placeholder)
        static_cast<uint32_t>(config_.access_token_ttl.count()));
    builder.Finish(resp);
    send_response(msg_ids::LOGIN_RESPONSE, corr_id, core_id, session_id,
                  {builder.GetBufferPointer(), builder.GetSize()});
}

void AuthService::handle_logout(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id)
{
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<apex::auth_svc::schemas::LogoutRequest>()) {
        spdlog::error("[AuthService] LogoutRequest verification failed");
        flatbuffers::FlatBufferBuilder builder(64);
        auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
            builder,
            apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
        builder.Finish(resp);
        send_response(msg_ids::LOGOUT_RESPONSE, corr_id, core_id, session_id,
                      {builder.GetBufferPointer(), builder.GetSize()});
        return;
    }

    auto* req = flatbuffers::GetRoot<apex::auth_svc::schemas::LogoutRequest>(
        fbs_payload.data());

    if (!req->access_token()) {
        flatbuffers::FlatBufferBuilder builder(64);
        auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
            builder,
            apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
        builder.Finish(resp);
        send_response(msg_ids::LOGOUT_RESPONSE, corr_id, core_id, session_id,
                      {builder.GetBufferPointer(), builder.GetSize()});
        return;
    }

    auto access_token = std::string(req->access_token()->string_view());

    spdlog::info("[AuthService] handle_logout (corr_id: {}, session: {})",
                 corr_id, session_id);

    // NOTE: Full logout flow (async):
    // 1. Verify token -> extract user_id
    // 2. Blacklist token in Redis (SETEX with remaining TTL)
    // 3. Delete Redis session
    // 4. Send LogoutResponse

    flatbuffers::FlatBufferBuilder builder(64);
    auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
        builder,
        apex::auth_svc::schemas::LogoutError_NONE);
    builder.Finish(resp);
    send_response(msg_ids::LOGOUT_RESPONSE, corr_id, core_id, session_id,
                  {builder.GetBufferPointer(), builder.GetSize()});
}

void AuthService::handle_refresh_token(
    std::span<const uint8_t> fbs_payload,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id)
{
    spdlog::info("[AuthService] handle_refresh_token (corr_id: {}, session: {})",
                 corr_id, session_id);

    // NOTE: Full refresh flow (async):
    // 1. Parse RefreshTokenRequest (shared schema)
    // 2. Hash token -> look up in PG refresh_tokens table
    // 3. Check revoked_at (Reuse Detection)
    // 4. Check expires_at
    // 5. Rotate: revoke old, issue new refresh_token
    // 6. Issue new access_token
    // 7. Send RefreshTokenResponse

    // Placeholder: send error (not yet implemented)
    // RefreshTokenResponse uses shared schema -- will be generated from
    // apex_shared/schemas/refresh_token_response.fbs
    (void)fbs_payload;
    (void)corr_id;
    (void)core_id;
    (void)session_id;
}

void AuthService::send_response(
    uint32_t msg_id,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload)
{
    // Build RoutingHeader
    envelope::RoutingHeader routing;
    routing.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    routing.flags = envelope::routing_flags::DIRECTION_RESPONSE;
    routing.msg_id = msg_id;

    // Build MetadataPrefix
    envelope::MetadataPrefix metadata;
    metadata.meta_version = envelope::MetadataPrefix::CURRENT_VERSION;
    metadata.core_id = core_id;
    metadata.corr_id = corr_id;
    metadata.source_id = envelope::source_ids::AUTH;
    metadata.session_id = session_id;
    metadata.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Serialize envelope
    auto routing_bytes = routing.serialize();
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
        spdlog::error("[AuthService] Failed to produce response (msg_id: {}, corr_id: {})",
                      msg_id, corr_id);
    }
}

} // namespace apex::auth_svc
