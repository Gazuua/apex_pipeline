#include <apex/auth_svc/auth_service.hpp>
#include <apex/auth_svc/crypto_util.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/protocols/kafka/envelope_builder.hpp>

// Generated FlatBuffers headers
#include <login_request_generated.h>
#include <login_response_generated.h>
#include <logout_request_generated.h>
#include <logout_response_generated.h>
#include <generated/refresh_token_request_generated.h>
#include <generated/refresh_token_response_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <array>
#include <charconv>
#include <chrono>
#include <format>

namespace apex::auth_svc {

namespace envelope = apex::shared::protocols::kafka;

// ============================================================
// Construction / Lifecycle
// ============================================================

AuthService::AuthService(AuthConfig config)
    : ServiceBase("auth")
    , config_(std::move(config))
    , jwt_manager_(config_.jwt_private_key_path,
                   config_.jwt_public_key_path,
                   config_.jwt_issuer,
                   config_.access_token_ttl)
    , password_hasher_(config_.bcrypt_work_factor)
{}

AuthService::~AuthService() = default;

void AuthService::on_configure(apex::core::ConfigureContext& ctx) {
    // Phase 1: 어댑터 참조 획득
    kafka_ = &ctx.server.adapter<apex::shared::adapters::kafka::KafkaAdapter>();
    redis_ = &ctx.server.adapter<apex::shared::adapters::redis::RedisAdapter>();
    pg_ = &ctx.server.adapter<apex::shared::adapters::pg::PgAdapter>();

    // Redis 바인딩 후 SessionStore 생성
    session_store_ = std::make_unique<SessionStore>(
        *redis_,
        config_.redis_session_prefix,
        config_.redis_blacklist_prefix,
        config_.session_ttl);

    spdlog::info("[AuthService] on_configure: 어댑터 바인딩 완료 (core_id={})",
                 ctx.core_id);
}

void AuthService::on_start() {
    spdlog::info("[AuthService] on_start: kafka_route 핸들러 등록");

    // kafka_route로 FlatBuffers 타입 핸들러 등록
    kafka_route<apex::auth_svc::schemas::LoginRequest>(
        msg_ids::LOGIN_REQUEST, &AuthService::on_login);
    kafka_route<apex::auth_svc::schemas::LogoutRequest>(
        msg_ids::LOGOUT_REQUEST, &AuthService::on_logout);
    kafka_route<apex::shared::schemas::RefreshTokenRequest>(
        msg_ids::REFRESH_TOKEN_REQUEST, &AuthService::on_refresh_token);

    spdlog::info("[AuthService] on_start 완료. Consuming from: {}",
                 config_.request_topic);
}

void AuthService::on_stop() {
    spdlog::info("[AuthService] on_stop");
}

// ============================================================
// Handler: Login
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> AuthService::on_login(
    const envelope::MetadataPrefix& meta,
    uint32_t /*msg_id*/,
    const apex::auth_svc::schemas::LoginRequest* req)
{
    // FlatBuffers 검증은 kafka_route가 자동 수행 — 실패 시 여기에 도달하지 않음.
    // 필드 null 체크만 수행.
    if (!req->email() || !req->password()) {
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = apex::auth_svc::schemas::CreateLoginResponse(
            builder,
            apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
        builder.Finish(resp);
        send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      {});  // reply_topic은 현재 메타에서 추출 불가 — 빈 문자열로 fallback
        co_return apex::core::ok();
    }

    // co_await 전에 필요한 데이터를 로컬에 복사 (FlatBuffers 포인터는 co_await 이후 무효)
    auto email = std::string(req->email()->string_view());
    auto password = std::string(req->password()->string_view());

    spdlog::info("[AuthService] on_login (corr_id: {}, session: {})",
                 meta.corr_id, meta.session_id);

    // --- Step 1: PG query — lookup user by email ---
    std::array<std::string, 1> login_params = {email};
    auto user_result = co_await pg_->query(
        "SELECT id, password_hash, locked_until FROM users WHERE email = $1",
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
                      {});
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
                      {});
        co_return apex::core::ok();
    }

    // --- Step 2: Extract user data ---
    // Columns: id(0), password_hash(1), locked(2)
    auto user_id_str = pg_res.value(0, 0);
    auto password_hash = pg_res.value(0, 1);

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
                          {});
            co_return apex::core::ok();
        }
    }
    bool is_locked = !pg_res.is_null(0, 2);  // locked_until IS NOT NULL → account locked

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
                      {});
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
                      {});
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
                      {});
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
                      {});
        co_return apex::core::ok();
    }
    auto& refresh_token = *refresh_token_result;

    // --- Step 7: Store refresh token hash in PG ---
    auto refresh_hash = sha256_hex(refresh_token);
    auto user_id_s = std::to_string(user_id);
    auto ttl_s = std::to_string(config_.refresh_token_ttl.count());
    std::array<std::string, 3> rt_params = {refresh_hash, user_id_s, ttl_s};
    auto rt_result = co_await pg_->execute(
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
    auto session_result = co_await session_store_->set(user_id, session_data);
    if (!session_result.has_value()) {
        spdlog::error("[AuthService] Failed to create Redis session: {}",
                      apex::core::error_code_name(session_result.error()));
        // Non-fatal: login still succeeds
    }

    // Store session:user:{uid} -> session_id (integer string)
    // Used by other services (e.g., Chat whisper) for online check + unicast routing
    co_await session_store_->set_user_session_id(user_id, meta.session_id);

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
                  {});

    spdlog::info("[AuthService] Login success (user_id: {}, email: {})", user_id, email);
    co_return apex::core::ok();
}

// ============================================================
// Handler: Logout
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> AuthService::on_logout(
    const envelope::MetadataPrefix& meta,
    uint32_t /*msg_id*/,
    const apex::auth_svc::schemas::LogoutRequest* req)
{
    if (!req->access_token()) {
        flatbuffers::FlatBufferBuilder builder(64);
        auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
            builder,
            apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
        builder.Finish(resp);
        send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      {});
        co_return apex::core::ok();
    }

    // co_await 전에 로컬 복사
    auto access_token = std::string(req->access_token()->string_view());

    spdlog::info("[AuthService] on_logout (corr_id: {}, session: {})",
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
                      {});
        co_return apex::core::ok();
    }

    auto& claims = *verify_result;

    // --- Step 2: Blacklist token by jti (matches Gateway's jwt:blacklist:{jti} lookup) ---
    auto remaining = jwt_manager_.remaining_ttl(access_token);
    if (remaining.count() > 0 && !claims.jti.empty()) {
        auto bl_result = co_await session_store_->blacklist_token(claims.jti, remaining);
        if (!bl_result.has_value()) {
            spdlog::error("[AuthService] Failed to blacklist token: {}",
                          apex::core::error_code_name(bl_result.error()));
            // Non-fatal: proceed with logout
        }
    }

    // --- Step 3: Remove Redis sessions ---
    auto remove_result = co_await session_store_->remove(claims.user_id);
    if (!remove_result.has_value()) {
        spdlog::error("[AuthService] Failed to remove session: {}",
                      apex::core::error_code_name(remove_result.error()));
        // Non-fatal: token is blacklisted, session will expire naturally
    }
    // Also remove session:user:{uid} mapping
    co_await session_store_->remove_user_session_id(claims.user_id);

    // --- Step 4: Send success response ---
    flatbuffers::FlatBufferBuilder builder(64);
    auto resp = apex::auth_svc::schemas::CreateLogoutResponse(
        builder, apex::auth_svc::schemas::LogoutError_NONE);
    builder.Finish(resp);
    send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id,
                  meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()},
                  {});

    spdlog::info("[AuthService] Logout success (user_id: {})", claims.user_id);
    co_return apex::core::ok();
}

// ============================================================
// Handler: Refresh Token
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> AuthService::on_refresh_token(
    const envelope::MetadataPrefix& meta,
    uint32_t /*msg_id*/,
    const apex::shared::schemas::RefreshTokenRequest* req)
{
    namespace rt_schemas = apex::shared::schemas;

    spdlog::info("[AuthService] on_refresh_token (corr_id: {}, session: {})",
                 meta.corr_id, meta.session_id);

    if (!req->refresh_token()) {
        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_INVALID);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      {});
        co_return apex::core::ok();
    }

    // co_await 전에 로컬 복사
    auto refresh_token_str = std::string(req->refresh_token()->string_view());

    // --- Step 2: Hash token and lookup in PG ---
    auto token_hash = sha256_hex(refresh_token_str);
    std::array<std::string, 1> lookup_params = {token_hash};
    auto lookup_result = co_await pg_->query(
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
                      {});
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
                      {});
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
                          {});
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
        auto revoke_all = co_await pg_->execute(
            "UPDATE refresh_tokens SET revoked_at = NOW() "
            "WHERE token_family = $1 AND revoked_at IS NULL",
            family_params);
        (void)revoke_all;  // Best-effort

        // Also remove Redis sessions for safety
        auto remove_result = co_await session_store_->remove(user_id);
        (void)remove_result;
        co_await session_store_->remove_user_session_id(user_id);

        flatbuffers::FlatBufferBuilder builder(128);
        auto resp = rt_schemas::CreateRefreshTokenResponse(
            builder, rt_schemas::RefreshTokenError_TOKEN_REVOKED);
        builder.Finish(resp);
        send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id,
                      meta.session_id,
                      {builder.GetBufferPointer(), builder.GetSize()},
                      {});
        co_return apex::core::ok();
    }

    // --- Step 4: Expiry check ---
    // expires_at is checked in SQL for simplicity (PG timestamp comparison).
    // We do a separate query to avoid parsing timestamps in C++.
    std::array<std::string, 1> expiry_params = {token_hash};
    auto expiry_result = co_await pg_->query(
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
                      {});
        co_return apex::core::ok();
    }

    // --- Step 5: Token Rotation — revoke old, issue new ---
    // Revoke the current refresh token
    std::array<std::string, 1> revoke_params = {token_hash};
    auto revoke_result = co_await pg_->execute(
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
                      {});
        co_return apex::core::ok();
    }
    auto& new_refresh_token = *new_rt_result;
    auto new_refresh_hash = sha256_hex(new_refresh_token);

    // Store new refresh token in PG (same family for reuse detection)
    auto user_id_s = std::to_string(user_id);
    auto ttl_s = std::to_string(config_.refresh_token_ttl.count());
    std::array<std::string, 4> insert_params = {
        new_refresh_hash, user_id_s, ttl_s, token_family};
    auto insert_result = co_await pg_->execute(
        "INSERT INTO refresh_tokens (token_hash, user_id, expires_at, token_family) "
        "VALUES ($1, $2, NOW() + make_interval(secs => $3::int), $4)",
        insert_params);
    (void)insert_result;  // Best-effort

    // --- Step 6: Issue new access token ---
    // We need user email for JWT claims — query from PG
    std::array<std::string, 1> email_params = {user_id_s};
    auto email_result = co_await pg_->query(
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
                      {});
        co_return apex::core::ok();
    }

    // Update Redis session
    auto session_data = std::format("uid:{}|email:{}|created:{}",
        user_id, user_email,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto session_set = co_await session_store_->set(user_id, session_data);
    (void)session_set;  // Best-effort
    // Also refresh session:user:{uid} -> session_id mapping
    co_await session_store_->set_user_session_id(user_id, meta.session_id);

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
                  {});

    spdlog::info("[AuthService] Refresh token rotated (user_id: {})", user_id);
    co_return apex::core::ok();
}

// ============================================================
// Response Builder (EnvelopeBuilder 사용)
// ============================================================

void AuthService::send_response(
    uint32_t msg_id,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload,
    const std::string& reply_topic)
{
    // EnvelopeBuilder 사용 — 힙 할당 (Auth 서비스는 bump 컨텍스트 불필요)
    // timestamp는 EnvelopeBuilder가 자동 설정 (epoch ms)
    auto envelope_buf = envelope::EnvelopeBuilder{}
        .routing(msg_id, envelope::routing_flags::DIRECTION_RESPONSE)
        .metadata(core_id, corr_id, envelope::source_ids::AUTH,
                  session_id, 0)
        .payload(fbs_payload)
        .build();

    // Reply-To: reply_topic이 있으면 그쪽으로 응답, 없으면 fallback
    const auto& target_topic = reply_topic.empty()
        ? config_.response_topic : reply_topic;

    // Use session_id as Kafka key (design doc section 7.1)
    auto key = std::to_string(session_id);
    auto result = kafka_->produce(
        target_topic,
        key,
        std::span<const uint8_t>(envelope_buf));

    if (!result.has_value()) {
        spdlog::error("[AuthService] Failed to produce response to '{}' (msg_id: {}, corr_id: {})",
                      target_topic, msg_id, corr_id);
    }
}

} // namespace apex::auth_svc
