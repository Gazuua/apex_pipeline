// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/auth_service.hpp>
#include <apex/auth_svc/crypto_util.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/protocols/kafka/envelope_builder.hpp>

// Generated FlatBuffers headers
#include <generated/refresh_token_request_generated.h>
#include <generated/refresh_token_response_generated.h>
#include <login_request_generated.h>
#include <login_response_generated.h>
#include <logout_request_generated.h>
#include <logout_response_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <array>
#include <charconv>
#include <chrono>
#include <format>

namespace apex::auth_svc
{

namespace envelope = apex::shared::protocols::kafka;

// ============================================================
// Construction / Lifecycle
// ============================================================

AuthService::AuthService(AuthConfig config)
    : ServiceBase("auth")
    , config_(std::move(config))
    , jwt_manager_(config_.jwt_private_key_path, config_.jwt_public_key_path, config_.jwt_issuer,
                   config_.access_token_ttl)
    , password_hasher_(config_.bcrypt_work_factor)
{}

AuthService::~AuthService() = default;

void AuthService::on_configure(apex::core::ConfigureContext& ctx)
{
    // Phase 1: 어댑터 참조 획득
    kafka_ = &ctx.server.adapter<apex::shared::adapters::kafka::KafkaAdapter>();
    redis_ = &ctx.server.adapter<apex::shared::adapters::redis::RedisAdapter>();
    pg_ = &ctx.server.adapter<apex::shared::adapters::pg::PgAdapter>();

    // Redis 바인딩 후 SessionStore 생성
    session_store_ = std::make_unique<SessionStore>(*redis_, config_.redis_session_prefix,
                                                    config_.redis_blacklist_prefix, config_.session_ttl);

    spdlog::info("[AuthService] on_configure: 어댑터 바인딩 완료 (core_id={})", ctx.core_id);
}

void AuthService::on_start()
{
    spdlog::info("[AuthService] on_start: kafka_route 핸들러 등록");

    // kafka_route로 FlatBuffers 타입 핸들러 등록
    kafka_route<apex::auth_svc::schemas::LoginRequest>(msg_ids::LOGIN_REQUEST, &AuthService::on_login);
    kafka_route<apex::auth_svc::schemas::LogoutRequest>(msg_ids::LOGOUT_REQUEST, &AuthService::on_logout);
    kafka_route<apex::shared::schemas::RefreshTokenRequest>(msg_ids::REFRESH_TOKEN_REQUEST,
                                                            &AuthService::on_refresh_token);

    // 테스트 사용자 패스워드 시딩 (E2E) — spawn()으로 tracked 코루틴 실행
    if (pg_)
    {
        spawn([this]() -> boost::asio::awaitable<void> {
            auto hash = password_hasher_.hash("password123");
            if (!hash.empty())
            {
                std::array<std::string, 1> params = {hash};
                auto result = co_await pg_->execute(
                    "UPDATE users SET password_hash = $1 WHERE password_hash = 'PENDING'", params);
                if (result.has_value())
                {
                    spdlog::info("[AuthService] Seeded test user passwords");
                }
                else
                {
                    spdlog::warn("[AuthService] Password seed failed (may already be set)");
                }
            }
        });
    }

    spdlog::info("[AuthService] on_start 완료. Consuming from: {}", config_.request_topic);
}

void AuthService::on_stop()
{
    spdlog::info("[AuthService] on_stop — cleaning up");

    // SessionStore 해제 — shutdown 후 Redis 접근 방지
    session_store_.reset();

    // 어댑터 참조를 null로 리셋 — shutdown 후 dangling pointer 접근 방지
    kafka_ = nullptr;
    redis_ = nullptr;
    pg_ = nullptr;
}

// ============================================================
// Handler: Login
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> AuthService::on_login(const apex::core::KafkaMessageMeta& meta,
                                                                       uint32_t /*msg_id*/,
                                                                       const apex::auth_svc::schemas::LoginRequest* req)
{
    // FlatBuffers 검증은 kafka_route가 자동 수행 — 실패 시 여기에 도달하지 않음.
    // 필드 null 체크만 수행.
    if (!req->email() || !req->password())
    {
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
    }

    // co_await 전에 필요한 데이터를 로컬에 복사 (FlatBuffers 포인터는 co_await 이후 무효)
    auto email = std::string(req->email()->string_view());
    auto password = std::string(req->password()->string_view());

    spdlog::info("[AuthService] on_login (corr_id: {}, session: {})", meta.corr_id, meta.session_id);

    // --- Step 1: PG query — lookup user by email ---
    std::array<std::string, 1> login_params = {email};
    auto user_result =
        co_await pg_->query("SELECT id, password_hash, locked_until FROM users WHERE email = $1", login_params);

    if (!user_result.has_value())
    {
        spdlog::error("[AuthService] PG query failed for login: {}", apex::core::error_code_name(user_result.error()));
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
    }

    auto& pg_res = *user_result;
    if (pg_res.row_count() == 0)
    {
        spdlog::warn("[AuthService] Login failed: user not found (email: {})", email);
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
    }

    // --- Step 2: Extract user data ---
    // Columns: id(0), password_hash(1), locked(2)
    auto user_id_str = pg_res.value(0, 0);
    auto password_hash = pg_res.value(0, 1);

    uint64_t user_id = 0;
    {
        auto [ptr, ec] = std::from_chars(user_id_str.data(), user_id_str.data() + user_id_str.size(), user_id);
        if (ec != std::errc{})
        {
            spdlog::error("[AuthService] Failed to parse user_id: '{}'", user_id_str);
            co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
        }
    }
    bool is_locked = !pg_res.is_null(0, 2); // locked_until IS NOT NULL → account locked

    // --- Step 3: Account lock check ---
    if (is_locked)
    {
        spdlog::warn("[AuthService] Login failed: account locked (user_id: {})", user_id);
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_ACCOUNT_LOCKED);
    }

    // --- Step 4: bcrypt password verification ---
    if (!password_hasher_.verify(password, password_hash))
    {
        spdlog::warn("[AuthService] Login failed: bad credentials (user_id: {})", user_id);
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_BAD_CREDENTIALS);
    }

    // --- Step 5: JWT access token issuance ---
    auto access_token = jwt_manager_.create_access_token(user_id, email);
    if (access_token.empty())
    {
        spdlog::error("[AuthService] Failed to create access token (user_id: {})", user_id);
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
    }

    // --- Step 6: Opaque refresh token generation ---
    auto refresh_token_result = generate_secure_token();
    if (!refresh_token_result.has_value())
    {
        spdlog::error("[AuthService] Failed to generate refresh token (CSPRNG failure)");
        co_return co_await send_login_error(meta, apex::auth_svc::schemas::LoginError_INTERNAL_ERROR);
    }
    auto& refresh_token = *refresh_token_result;

    // --- Step 7: Store refresh token hash in PG ---
    auto refresh_hash = sha256_hex(refresh_token);
    auto user_id_s = std::to_string(user_id);
    auto ttl_s = std::to_string(config_.refresh_token_ttl.count());
    std::array<std::string, 3> rt_params = {refresh_hash, user_id_s, ttl_s};
    auto rt_result = co_await pg_->execute("INSERT INTO refresh_tokens (token_hash, user_id, expires_at) "
                                           "VALUES ($1, $2, NOW() + make_interval(secs => $3::int))",
                                           rt_params);

    if (!rt_result.has_value())
    {
        spdlog::error("[AuthService] Failed to store refresh token in PG: {}",
                      apex::core::error_code_name(rt_result.error()));
        // Non-fatal: login still succeeds, refresh may fail later
    }

    // --- Step 8: Redis session creation ---
    // Store detailed session under auth:session:{uid}
    auto session_data = std::format(
        "uid:{}|email:{}|created:{}", user_id, email,
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto session_result = co_await session_store_->set(user_id, session_data);
    if (!session_result.has_value())
    {
        spdlog::error("[AuthService] Failed to create Redis session: {}",
                      apex::core::error_code_name(session_result.error()));
        // Non-fatal: login still succeeds
    }

    // Store session:user:{uid} -> session_id (integer string)
    // Used by other services (e.g., Chat whisper) for online check + unicast routing
    auto user_session_result = co_await session_store_->set_user_session_id(user_id, meta.session_id);
    if (!user_session_result.has_value())
    {
        spdlog::error("[AuthService] set_user_session_id failed for user_id={}: {}", user_id,
                      apex::core::error_code_name(user_session_result.error()));
    }

    // --- Step 9: Build and send LoginResponse ---
    flatbuffers::FlatBufferBuilder builder(512);
    auto at_off = builder.CreateString(access_token);
    auto rt_off = builder.CreateString(refresh_token);
    auto resp =
        apex::auth_svc::schemas::CreateLoginResponse(builder, apex::auth_svc::schemas::LoginError_NONE, at_off, rt_off,
                                                     user_id, static_cast<uint32_t>(config_.access_token_ttl.count()));
    builder.Finish(resp);
    send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()}, {});

    spdlog::info("[AuthService] Login success (user_id: {}, email: {})", user_id, email);
    co_return apex::core::ok();
}

// ============================================================
// Handler: Logout
// ============================================================

boost::asio::awaitable<apex::core::Result<void>>
AuthService::on_logout(const apex::core::KafkaMessageMeta& meta, uint32_t /*msg_id*/,
                       const apex::auth_svc::schemas::LogoutRequest* req)
{
    if (!req->access_token())
    {
        co_return co_await send_logout_error(meta, apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
    }

    // co_await 전에 로컬 복사
    auto access_token = std::string(req->access_token()->string_view());

    spdlog::info("[AuthService] on_logout (corr_id: {}, session: {})", meta.corr_id, meta.session_id);

    // --- Step 1: JWT verification — extract claims ---
    auto verify_result = jwt_manager_.verify_access_token(access_token);
    if (!verify_result.has_value())
    {
        spdlog::warn("[AuthService] Logout failed: invalid token");
        co_return co_await send_logout_error(meta, apex::auth_svc::schemas::LogoutError_INVALID_TOKEN);
    }

    auto& claims = *verify_result;

    // --- Step 2: Blacklist token by jti (matches Gateway's jwt:blacklist:{jti} lookup) ---
    auto remaining = jwt_manager_.remaining_ttl(access_token);
    if (remaining.count() > 0 && !claims.jti.empty())
    {
        auto bl_result = co_await session_store_->blacklist_token(claims.jti, remaining);
        if (!bl_result.has_value())
        {
            spdlog::error("[AuthService] Failed to blacklist token: {}",
                          apex::core::error_code_name(bl_result.error()));
            // Non-fatal: proceed with logout
        }
    }

    // --- Step 3: Remove Redis sessions ---
    auto remove_result = co_await session_store_->remove(claims.user_id);
    if (!remove_result.has_value())
    {
        spdlog::error("[AuthService] Failed to remove session: {}", apex::core::error_code_name(remove_result.error()));
        // Non-fatal: token is blacklisted, session will expire naturally
    }
    // Also remove session:user:{uid} mapping
    auto remove_user_session = co_await session_store_->remove_user_session_id(claims.user_id);
    if (!remove_user_session.has_value())
    {
        spdlog::error("[AuthService] remove_user_session_id failed for user_id={}: {}", claims.user_id,
                      apex::core::error_code_name(remove_user_session.error()));
    }

    // --- Step 4: Send success response ---
    flatbuffers::FlatBufferBuilder builder(64);
    auto resp = apex::auth_svc::schemas::CreateLogoutResponse(builder, apex::auth_svc::schemas::LogoutError_NONE);
    builder.Finish(resp);
    send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()}, {});

    spdlog::info("[AuthService] Logout success (user_id: {})", claims.user_id);
    co_return apex::core::ok();
}

// ============================================================
// Handler: Refresh Token
// ============================================================

boost::asio::awaitable<apex::core::Result<void>>
AuthService::on_refresh_token(const apex::core::KafkaMessageMeta& meta, uint32_t /*msg_id*/,
                              const apex::shared::schemas::RefreshTokenRequest* req)
{
    namespace rt_schemas = apex::shared::schemas;

    spdlog::info("[AuthService] on_refresh_token (corr_id: {}, session: {})", meta.corr_id, meta.session_id);

    if (!req->refresh_token())
    {
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_TOKEN_INVALID);
    }

    // co_await 전에 로컬 복사
    auto refresh_token_str = std::string(req->refresh_token()->string_view());

    // --- Step 2: Hash token and lookup in PG ---
    auto token_hash = sha256_hex(refresh_token_str);
    std::array<std::string, 1> lookup_params = {token_hash};
    auto lookup_result = co_await pg_->query("SELECT user_id, revoked_at, expires_at, token_family "
                                             "FROM refresh_tokens WHERE token_hash = $1",
                                             lookup_params);

    if (!lookup_result.has_value())
    {
        spdlog::error("[AuthService] PG query failed for refresh token: {}",
                      apex::core::error_code_name(lookup_result.error()));
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
    }

    auto& pg_res = *lookup_result;
    if (pg_res.row_count() == 0)
    {
        spdlog::warn("[AuthService] Refresh token not found");
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_TOKEN_INVALID);
    }

    // Columns: user_id(0), revoked_at(1), expires_at(2), token_family(3)
    auto user_id_str = pg_res.value(0, 0);
    uint64_t user_id = 0;
    {
        auto [ptr, ec] = std::from_chars(user_id_str.data(), user_id_str.data() + user_id_str.size(), user_id);
        if (ec != std::errc{})
        {
            spdlog::error("[AuthService] Failed to parse user_id in refresh: '{}'", user_id_str);
            co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
        }
    }
    bool is_revoked = !pg_res.is_null(0, 1); // revoked_at != NULL
    auto token_family_sv = pg_res.value(0, 3);
    auto token_family = std::string(token_family_sv);

    // --- Step 3: Reuse Detection ---
    // If this token has already been revoked, it's a reuse attempt.
    // Revoke ALL tokens in the same family for security.
    if (is_revoked)
    {
        spdlog::warn("[AuthService] Refresh token reuse detected! "
                     "Revoking entire token family (user_id: {})",
                     user_id);
        // Revoke all tokens in this family
        std::array<std::string, 1> family_params = {token_family};
        auto revoke_all = co_await pg_->execute("UPDATE refresh_tokens SET revoked_at = NOW() "
                                                "WHERE token_family = $1 AND revoked_at IS NULL",
                                                family_params);
        if (!revoke_all.has_value())
        {
            spdlog::warn("[AuthService] Best-effort revoke_all failed: {}",
                         apex::core::error_code_name(revoke_all.error()));
        }

        // Also remove Redis sessions for safety
        auto remove_result = co_await session_store_->remove(user_id);
        if (!remove_result.has_value())
        {
            spdlog::error("[AuthService] Failed to remove session during reuse detection (user_id={}): {}", user_id,
                          apex::core::error_code_name(remove_result.error()));
        }
        auto remove_user_sid = co_await session_store_->remove_user_session_id(user_id);
        if (!remove_user_sid.has_value())
        {
            spdlog::error("[AuthService] remove_user_session_id failed for user_id={}: {}", user_id,
                          apex::core::error_code_name(remove_user_sid.error()));
        }

        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_TOKEN_REVOKED);
    }

    // --- Step 4: Expiry check ---
    // expires_at is checked in SQL for simplicity (PG timestamp comparison).
    // We do a separate query to avoid parsing timestamps in C++.
    std::array<std::string, 1> expiry_params = {token_hash};
    auto expiry_result =
        co_await pg_->query("SELECT 1 FROM refresh_tokens WHERE token_hash = $1 AND expires_at < NOW()", expiry_params);

    if (!expiry_result.has_value())
    {
        spdlog::error("[AuthService] PG expiry check query failed: {}",
                      apex::core::error_code_name(expiry_result.error()));
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
    }

    if (expiry_result->row_count() > 0)
    {
        spdlog::warn("[AuthService] Refresh token expired (user_id: {})", user_id);
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_TOKEN_EXPIRED);
    }

    // --- Step 5: Token Rotation — revoke old, issue new ---
    // Revoke the current refresh token
    std::array<std::string, 1> revoke_params = {token_hash};
    auto revoke_result =
        co_await pg_->execute("UPDATE refresh_tokens SET revoked_at = NOW() WHERE token_hash = $1", revoke_params);
    if (!revoke_result.has_value())
    {
        spdlog::warn("[AuthService] Best-effort revoke_result failed: {}",
                     apex::core::error_code_name(revoke_result.error()));
    }

    // Generate new refresh token
    auto new_rt_result = generate_secure_token();
    if (!new_rt_result.has_value())
    {
        spdlog::error("[AuthService] Failed to generate new refresh token");
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
    }
    auto& new_refresh_token = *new_rt_result;
    auto new_refresh_hash = sha256_hex(new_refresh_token);

    // Store new refresh token in PG (same family for reuse detection)
    auto user_id_s = std::to_string(user_id);
    auto ttl_s = std::to_string(config_.refresh_token_ttl.count());
    std::array<std::string, 4> insert_params = {new_refresh_hash, user_id_s, ttl_s, token_family};
    auto insert_result =
        co_await pg_->execute("INSERT INTO refresh_tokens (token_hash, user_id, expires_at, token_family) "
                              "VALUES ($1, $2, NOW() + make_interval(secs => $3::int), $4)",
                              insert_params);
    if (!insert_result.has_value())
    {
        spdlog::error("[AuthService] Failed to insert new refresh token: {}",
                      apex::core::error_code_name(insert_result.error()));
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
    }

    // --- Step 6: Issue new access token ---
    // We need user email for JWT claims — query from PG
    std::array<std::string, 1> email_params = {user_id_s};
    auto email_result = co_await pg_->query("SELECT email FROM users WHERE id = $1", email_params);

    std::string user_email;
    if (email_result.has_value() && email_result->row_count() > 0)
    {
        user_email = std::string(email_result->value(0, 0));
    }
    if (user_email.empty())
    {
        spdlog::error("[AuthService] Failed to retrieve email for user_id={} during refresh", user_id);
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
    }

    auto new_access_token = jwt_manager_.create_access_token(user_id, user_email);
    if (new_access_token.empty())
    {
        spdlog::error("[AuthService] Failed to create access token for refresh (user_id: {})", user_id);
        co_return co_await send_refresh_token_error(meta, rt_schemas::RefreshTokenError_INTERNAL_ERROR);
    }

    // Update Redis session
    auto session_data = std::format(
        "uid:{}|email:{}|created:{}", user_id, user_email,
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto session_set = co_await session_store_->set(user_id, session_data);
    if (!session_set.has_value())
    {
        spdlog::error("[AuthService] Failed to update Redis session during refresh: {}",
                      apex::core::error_code_name(session_set.error()));
        // Non-fatal: refresh still succeeds
    }
    // Also refresh session:user:{uid} -> session_id mapping
    auto refresh_user_session = co_await session_store_->set_user_session_id(user_id, meta.session_id);
    if (!refresh_user_session.has_value())
    {
        spdlog::error("[AuthService] set_user_session_id failed for user_id={}: {}", user_id,
                      apex::core::error_code_name(refresh_user_session.error()));
    }

    // --- Step 7: Build and send RefreshTokenResponse ---
    flatbuffers::FlatBufferBuilder builder(512);
    auto at_off = builder.CreateString(new_access_token);
    auto nrt_off = builder.CreateString(new_refresh_token);
    auto resp =
        rt_schemas::CreateRefreshTokenResponse(builder, rt_schemas::RefreshTokenError_NONE, at_off,
                                               static_cast<uint32_t>(config_.access_token_ttl.count()), nrt_off);
    builder.Finish(resp);
    send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {builder.GetBufferPointer(), builder.GetSize()}, {});

    spdlog::info("[AuthService] Refresh token rotated (user_id: {})", user_id);
    co_return apex::core::ok();
}

// ============================================================
// Response Builder (EnvelopeBuilder 사용)
// ============================================================

void AuthService::send_response(uint32_t msg_id, uint64_t corr_id, uint16_t core_id, uint64_t session_id,
                                std::span<const uint8_t> fbs_payload, const std::string& reply_topic)
{
    // EnvelopeBuilder 사용 — 힙 할당 (Auth 서비스는 bump 컨텍스트 불필요)
    // timestamp는 EnvelopeBuilder가 자동 설정 (epoch ms)
    auto envelope_buf = envelope::EnvelopeBuilder{}
                            .routing(msg_id, envelope::routing_flags::DIRECTION_RESPONSE)
                            .metadata(core_id, corr_id, envelope::source_ids::AUTH, session_id, 0)
                            .payload(fbs_payload)
                            .build();

    // Reply-To: reply_topic이 있으면 그쪽으로 응답, 없으면 fallback
    const auto& target_topic = reply_topic.empty() ? config_.response_topic : reply_topic;

    // Use session_id as Kafka key (design doc section 7.1)
    auto key = std::to_string(session_id);
    auto result = kafka_->produce(target_topic, key, std::span<const uint8_t>(envelope_buf));

    if (!result.has_value())
    {
        spdlog::error("[AuthService] Failed to produce response to '{}' (msg_id: {}, corr_id: {})", target_topic,
                      msg_id, corr_id);
    }
}

// ============================================================
// Error Response Helpers [D5]
// ============================================================

boost::asio::awaitable<apex::core::Result<void>> AuthService::send_login_error(const apex::core::KafkaMessageMeta& meta,
                                                                               uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp =
        apex::auth_svc::schemas::CreateLoginResponse(fbb, static_cast<apex::auth_svc::schemas::LoginError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::LOGIN_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, {});
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
AuthService::send_logout_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    flatbuffers::FlatBufferBuilder fbb(64);
    auto resp =
        apex::auth_svc::schemas::CreateLogoutResponse(fbb, static_cast<apex::auth_svc::schemas::LogoutError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::LOGOUT_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, {});
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
AuthService::send_refresh_token_error(const apex::core::KafkaMessageMeta& meta, uint16_t error)
{
    namespace rt_schemas = apex::shared::schemas;
    flatbuffers::FlatBufferBuilder fbb(128);
    auto resp = rt_schemas::CreateRefreshTokenResponse(fbb, static_cast<rt_schemas::RefreshTokenError>(error));
    fbb.Finish(resp);
    send_response(msg_ids::REFRESH_TOKEN_RESPONSE, meta.corr_id, meta.core_id, meta.session_id,
                  {fbb.GetBufferPointer(), fbb.GetSize()}, {});
    co_return apex::core::ok();
}

} // namespace apex::auth_svc
