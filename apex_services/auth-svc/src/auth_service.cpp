#include <apex/auth_svc/auth_service.hpp>
#include <apex/auth_svc/crypto_util.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

// Generated FlatBuffers headers
#include <login_request_generated.h>
#include <login_response_generated.h>
#include <logout_request_generated.h>
#include <logout_response_generated.h>
#include <generated/refresh_token_request_generated.h>
#include <generated/refresh_token_response_generated.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <array>
#include <charconv>
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
    constexpr uint32_t REFRESH_TOKEN_REQUEST = 1004;
    constexpr uint32_t REFRESH_TOKEN_RESPONSE = 1005;
} // namespace msg_ids

AuthService::AuthService(
    AuthConfig config,
    boost::asio::any_io_executor executor,
    apex::shared::adapters::kafka::KafkaAdapter& kafka,
    apex::shared::adapters::redis::RedisAdapter& redis,
    apex::shared::adapters::pg::PgAdapter& pg)
    : config_(std::move(config))
    , executor_(std::move(executor))
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

    // Register actual handlers to MessageDispatcher (O(1) hash map).
    // Handlers access Kafka envelope metadata via this->current_meta_
    // (set in dispatch_envelope() before dispatcher_.dispatch()).
    dispatcher_.register_handler(msg_ids::LOGIN_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_login(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::LOGOUT_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_logout(std::move(session), msg_id, payload);
        });
    dispatcher_.register_handler(msg_ids::REFRESH_TOKEN_REQUEST,
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {
            co_return co_await handle_refresh_token(std::move(session), msg_id, payload);
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
    spdlog::info("[AuthService] Started. {} handlers registered. Consuming from: {}",
                 dispatcher_.handler_count(), config_.request_topic);
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

    // Parse MetadataPrefix (40B) starting at offset 8
    auto meta_result = envelope::MetadataPrefix::parse(
        payload.subspan(envelope::RoutingHeader::SIZE));
    if (!meta_result.has_value()) {
        spdlog::error("[AuthService] Failed to parse MetadataPrefix");
        return;
    }

    auto& routing = *routing_result;
    auto& metadata = *meta_result;

    // Check if handler exists (O(1) lookup via MessageDispatcher)
    if (!dispatcher_.has_handler(routing.msg_id)) {
        spdlog::warn("[AuthService] Unknown msg_id: {}", routing.msg_id);
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

boost::asio::awaitable<apex::core::Result<void>> AuthService::handle_login(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    // Read metadata from member cache (set in dispatch_envelope)
    auto meta = current_meta_;

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
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<apex::auth_svc::schemas::LoginRequest>(
        fbs_payload.data());

    if (!req->email() || !req->password()) {
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder,
            apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto email = std::string(req->email()->string_view());
    auto password = std::string(req->password()->string_view());

    spdlog::info("[AuthService] handle_login (corr_id: {}, session: {})",
                 meta.corr_id, meta.session_id);

    // --- Step 1: PG query — lookup user by email ---
    std::array<std::string, 1> login_params = {email};
    auto user_result = co_await pg_.query(
        "SELECT id, password_hash, locked FROM users WHERE email = $1",
        login_params);

    if (!user_result.has_value()) {
        spdlog::error("[AuthService] PG query failed for login: {}",
                      apex::core::error_code_name(user_result.error()));
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto& pg_res = *user_result;
    if (pg_res.row_count() == 0) {
        spdlog::warn("[AuthService] Login failed: user not found (email: {})", email);
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder, apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // --- Step 2: Extract user data ---
    // Columns: id(0), password_hash(1), locked(2)
    auto user_id_str = pg_res.value(0, 0);
    auto password_hash = pg_res.value(0, 1);
    auto locked_str = pg_res.value(0, 2);

    uint64_t user_id = 0;
    {
        auto [ptr, ec] = std::from_chars(
            user_id_str.data(), user_id_str.data() + user_id_str.size(), user_id);
        if (ec != std::errc{}) {
            spdlog::error("[AuthService] Failed to parse user_id: '{}'", user_id_str);
            flatbuffers::FlatBufferBuilder builder(128);
            auto resp = apex::auth_svc::schemas::CreateLoginResponse(
                builder, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
            builder.Finish(resp);
            send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                          meta.session_id,
                          {builder.GetBufferPointer(), builder.GetSize()},
                          meta.reply_topic);
            co_return apex::core::ok();
        }
    }
    bool is_locked = (locked_str == "t" || locked_str == "true" || locked_str == "1");

    // --- Step 3: Account lock check ---
    if (is_locked) {
        spdlog::warn("[AuthService] Login failed: account locked (user_id: {})", user_id);
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder, apex::auth_svc::schemas::LoginError_ACCOUNT_LOCKED);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // --- Step 4: bcrypt password verification ---
    if (!password_hasher_.verify(password, password_hash)) {
        spdlog::warn("[AuthService] Login failed: bad credentials (user_id: {})", user_id);
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder, apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // --- Step 5: JWT access token issuance ---
    auto access_token = jwt_manager_.create_access_token(user_id, email);
    if (access_token.empty()) {
        spdlog::error("[AuthService] Failed to create access token (user_id: {})", user_id);
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // --- Step 6: Opaque refresh token generation ---
    auto refresh_token_result = generate_secure_token();
    if (!refresh_token_result.has_value()) {
        spdlog::error("[AuthService] Failed to generate refresh token (CSPRNG failure)");
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }
    auto& refresh_token = *refresh_token_result;

    // --- Step 7: Store refresh token hash in PG ---
    auto refresh_hash = sha256_hex(refresh_token);
    auto user_id_s = std::to_string(user_id);
    auto ttl_s = std::to_string(config_.refresh_token_ttl.count());
    std::array<std::string, 3> rt_params = {refresh_hash, user_id_s, ttl_s};
    auto rt_result = co_await pg_.execute(
        "INSERT INTO refresh_tokens (token_hash, user_id, expires_at) "
        "VALUES ($1, $2, NOW() + make_interval(secs => $3::int))",
        rt_params);

    if (!rt_result.has_value()) {
        spdlog::error("[AuthService] Failed to store refresh token in PG: {}",
                      apex::core::error_code_name(rt_result.error()));
        // Non-fatal: login still succeeds, refresh may fail later
    }

    // --- Step 8: Redis session creation ---
    // Store detailed session under auth:session:{uid}
    auto session_data = std::format("uid:{}|email:{}|created:{}",
        user_id, email,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto session_result = co_await session_store_.set(user_id, session_data);
    if (!session_result.has_value()) {
        spdlog::error("[AuthService] Failed to create Redis session: {}",
                      apex::core::error_code_name(session_result.error()));
        // Non-fatal: login still succeeds
    }

    // Store session:user:{uid} -> session_id (integer string)
    // Used by other services (e.g., Chat whisper) for online check + unicast routing
    co_await session_store_.set_user_session_id(user_id, meta.session_id);

    // --- Step 9: Build and send LoginResponse ---
    flatbuffers::FlatBufferBuilder builder(512);
    auto at_off = builder.CreateString(access_token);
    auto rt_off = builder.CreateString(refresh_token);
    auto resp = apex::auth_svc::schemas::CreateLoginResponse(
        builder,
        apex::auth_svc::schemas::LoginError_NONE,
        at_off,
        rt_off,
        user_id,
        static_cast<uint32_t>(config_.access_token_ttl.count()));
    builder.Finish(resp);
    send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()},
                  meta.reply_topic);

    spdlog::info("[AuthService] Login success (user_id: {}, email: {})", user_id, email);
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> AuthService::handle_logout(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<apex::auth_svc::schemas::LogoutRequest>()) {
        spdlog::error("[AuthService] LogoutRequest verification failed");
        flatbuffers::FlatBufferBuilder builder(64);
        auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
            builder,
            apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
        builder.Finish(resp);
        send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<apex::auth_svc::schemas::LogoutRequest>(
        fbs_payload.data());

    if (!req->access_token()) {
        flatbuffers::FlatBufferBuilder builder(64);
        auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
            builder,
            apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
        builder.Finish(resp);
        send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto access_token = std::string(req->access_token()->string_view());

    spdlog::info("[AuthService] handle_logout (corr_id: {}, session: {})",
                 meta.corr_id, meta.session_id);

    // --- Step 1: JWT verification — extract claims ---
    auto verify_result = jwt_manager_.verify_access_token(access_token);
    if (!verify_result.has_value()) {
        spdlog::warn("[AuthService] Logout failed: invalid token");
        flatbuffers::FlatBufferBuilder builder(64);
        auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
            builder, apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
        builder.Finish(resp);
        send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto& claims = *verify_result;

    // --- Step 2: Blacklist token by jti (matches Gateway's jwt:blacklist:{jti} lookup) ---
    auto remaining = jwt_manager_.remaining_ttl(access_token);
    if (remaining.count() > 0 && !claims.jti.empty()) {
        auto bl_result = co_await session_store_.blacklist_token(claims.jti, remaining);
        if (!bl_result.has_value()) {
            spdlog::error("[AuthService] Failed to blacklist token: {}",
                          apex::core::error_code_name(bl_result.error()));
            // Non-fatal: proceed with logout
        }
    }

    // --- Step 3: Remove Redis sessions ---
    auto remove_result = co_await session_store_.remove(claims.user_id);
    if (!remove_result.has_value()) {
        spdlog::error("[AuthService] Failed to remove session: {}",
                      apex::core::error_code_name(remove_result.error()));
        // Non-fatal: token is blacklisted, session will expire naturally
    }
    // Also remove session:user:{uid} mapping
    co_await session_store_.remove_user_session_id(claims.user_id);

    // --- Step 4: Send success response ---
    flatbuffers::FlatBufferBuilder builder(64);
    auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
        builder, apex::auth_svc::schemas::LogoutError_NONE);
    builder.Finish(resp);
    send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()},
                  meta.reply_topic);

    spdlog::info("[AuthService] Logout success (user_id: {})", claims.user_id);
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> AuthService::handle_refresh_token(
    apex::core::SessionPtr /*session*/,
    uint32_t /*msg_id*/,
    std::span<const uint8_t> fbs_payload)
{
    auto meta = current_meta_;

    spdlog::info("[AuthService] handle_refresh_token (corr_id: {}, session: {})",
                 meta.corr_id, meta.session_id);

    namespace rt_schemas = apex::shared::schemas;

    // --- Step 1: FlatBuffers verify + parse ---
    flatbuffers::Verifier verifier(fbs_payload.data(), fbs_payload.size());
    if (!verifier.VerifyBuffer<rt_schemas::RefreshTokenRequest>()) {
        spdlog::error("[AuthService] RefreshTokenRequest verification failed");
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_INVALID);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto* req = flatbuffers::GetRoot<rt_schemas::RefreshTokenRequest>(
        fbs_payload.data());

    if (!req->refresh_token()) {
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_INVALID);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto refresh_token_str = std::string(req->refresh_token()->string_view());

    // --- Step 2: Hash token and lookup in PG ---
    auto token_hash = sha256_hex(refresh_token_str);
    std::array<std::string, 1> lookup_params = {token_hash};
    auto lookup_result = co_await pg_.query(
        "SELECT user_id, revoked_at, expires_at, token_family "
        "FROM refresh_tokens WHERE token_hash = $1",
        lookup_params);

    if (!lookup_result.has_value()) {
        spdlog::error("[AuthService] PG query failed for refresh token: {}",
                      apex::core::error_code_name(lookup_result.error()));
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    auto& pg_res = *lookup_result;
    if (pg_res.row_count() == 0) {
        spdlog::warn("[AuthService] Refresh token not found");
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_INVALID);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // Columns: user_id(0), revoked_at(1), expires_at(2), token_family(3)
    auto user_id_str = pg_res.value(0, 0);
    uint64_t user_id = 0;
    {
        auto [ptr, ec] = std::from_chars(
            user_id_str.data(), user_id_str.data() + user_id_str.size(), user_id);
        if (ec != std::errc{}) {
            spdlog::error("[AuthService] Failed to parse user_id in refresh: '{}'", user_id_str);
            flatbuffers::FlatBufferBuilder builder(128);
            auto resp = rt_schemas::CreateRefreshTokenResponse(
                builder, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
            builder.Finish(resp);
            send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                          meta.session_id,
                          {builder.GetBufferPointer(), builder.GetSize()},
                          meta.reply_topic);
            co_return apex::core::ok();
        }
    }
    bool is_revoked = !pg_res.is_null(0, 1);  // revoked_at != NULL
    auto token_family_sv = pg_res.value(0, 3);
    auto token_family = std::string(token_family_sv);

    // --- Step 3: Reuse Detection ---
    // If this token has already been revoked, it's a reuse attempt.
    // Revoke ALL tokens in the same family for security.
    if (is_revoked) {
        spdlog::warn("[AuthService] Refresh token reuse detected! "
                     "Revoking entire token family (user_id: {})", user_id);
        // Revoke all tokens in this family
        std::array<std::string, 1> family_params = {token_family};
        auto revoke_all = co_await pg_.execute(
            "UPDATE refresh_tokens SET revoked_at = NOW() "
            "WHERE token_family = $1 AND revoked_at IS NULL",
            family_params);
        (void)revoke_all;  // Best-effort

        // Also remove Redis sessions for safety
        auto remove_result = co_await session_store_.remove(user_id);
        (void)remove_result;
        co_await session_store_.remove_user_session_id(user_id);

        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_REVOKED);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // --- Step 4: Expiry check ---
    // expires_at is checked in SQL for simplicity (PG timestamp comparison).
    // We do a separate query to avoid parsing timestamps in C++.
    std::array<std::string, 1> expiry_params = {token_hash};
    auto expiry_result = co_await pg_.query(
        "SELECT 1 FROM refresh_tokens WHERE token_hash = $1 AND expires_at < NOW()",
        expiry_params);

    if (expiry_result.has_value() && expiry_result->row_count() > 0) {
        spdlog::warn("[AuthService] Refresh token expired (user_id: {})", user_id);
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_EXPIRED);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // --- Step 5: Token Rotation — revoke old, issue new ---
    // Revoke the current refresh token
    std::array<std::string, 1> revoke_params = {token_hash};
    auto revoke_result = co_await pg_.execute(
        "UPDATE refresh_tokens SET revoked_at = NOW() WHERE token_hash = $1",
        revoke_params);
    (void)revoke_result;  // Best-effort

    // Generate new refresh token
    auto new_rt_result = generate_secure_token();
    if (!new_rt_result.has_value()) {
        spdlog::error("[AuthService] Failed to generate new refresh token");
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }
    auto& new_refresh_token = *new_rt_result;
    auto new_refresh_hash = sha256_hex(new_refresh_token);

    // Store new refresh token in PG (same family for reuse detection)
    auto user_id_s = std::to_string(user_id);
    auto ttl_s = std::to_string(config_.refresh_token_ttl.count());
    std::array<std::string, 4> insert_params = {
        new_refresh_hash, user_id_s, ttl_s, token_family};
    auto insert_result = co_await pg_.execute(
        "INSERT INTO refresh_tokens (token_hash, user_id, expires_at, token_family) "
        "VALUES ($1, $2, NOW() + make_interval(secs => $3::int), $4)",
        insert_params);
    (void)insert_result;  // Best-effort

    // --- Step 6: Issue new access token ---
    // We need user email for JWT claims — query from PG
    std::array<std::string, 1> email_params = {user_id_s};
    auto email_result = co_await pg_.query(
        "SELECT email FROM users WHERE id = $1", email_params);

    std::string user_email;
    if (email_result.has_value() && email_result->row_count() > 0) {
        user_email = std::string(email_result->value(0, 0));
    }

    auto new_access_token = jwt_manager_.create_access_token(user_id, user_email);
    if (new_access_token.empty()) {
        spdlog::error("[AuthService] Failed to create access token for refresh (user_id: {})", user_id);
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      meta.reply_topic);
        co_return apex::core::ok();
    }

    // Update Redis session
    auto session_data = std::format("uid:{}|email:{}|created:{}",
        user_id, user_email,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto session_set = co_await session_store_.set(user_id, session_data);
    (void)session_set;  // Best-effort
    // Also refresh session:user:{uid} -> session_id mapping
    co_await session_store_.set_user_session_id(user_id, meta.session_id);

    // --- Step 7: Build and send RefreshTokenResponse ---
    flatbuffers::FlatBufferBuilder builder(512);
    auto at_off = builder.CreateString(new_access_token);
    auto nrt_off = builder.CreateString(new_refresh_token);
    auto resp = rt_schemas::CreateRefreshTokenResponse(
        builder,
        rt_schemas::RefreshTokenError_NONE,
        at_off,
        static_cast<uint32_t>(config_.access_token_ttl.count()),
        nrt_off);
    builder.Finish(resp);
    send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()},
                  meta.reply_topic);

    spdlog::info("[AuthService] Refresh token rotated (user_id: {})", user_id);
    co_return apex::core::ok();
}

void AuthService::send_response(
    uint32_t msg_id,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload,
    const std::string& reply_topic)
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
        spdlog::error("[AuthService] Failed to produce response to '{}' (msg_id: {}, corr_id: {})",
                      target_topic, msg_id, corr_id);
    }
}

} // namespace apex::auth_svc
