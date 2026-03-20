// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/jwt_manager.hpp>
#include <apex/auth_svc/password_hasher.hpp>
#include <apex/auth_svc/session_store.hpp>
#include <apex/core/kafka_message_meta.hpp>
#include <apex/core/result.hpp>
#include <apex/core/service_base.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

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

// Forward declarations for FlatBuffers types
namespace apex::auth_svc::schemas
{
class LoginRequest;
class LogoutRequest;
} // namespace apex::auth_svc::schemas
namespace apex::shared::schemas
{
class RefreshTokenRequest;
}

namespace apex::auth_svc
{

/// Auth Service -- Kafka-based authentication request/response handler.
///
/// ServiceBase<AuthService> 상속으로 Server + kafka_route 패턴 사용.
/// on_configure()에서 어댑터 참조 획득, on_start()에서 kafka_route 핸들러 등록.
///
/// Consumes from `auth.requests` topic, processes login/logout/refresh,
/// and produces responses to `auth.responses` topic.
class AuthService : public apex::core::ServiceBase<AuthService>
{
  public:
    explicit AuthService(AuthConfig config);
    ~AuthService();

    // ── ServiceBase 라이프사이클 훅 ───────────────────────────────────────

    /// Phase 1: 어댑터 참조 획득 (Kafka, Redis, PG).
    void on_configure(apex::core::ConfigureContext& ctx) override;

    /// 핸들러 등록 (kafka_route).
    void on_start() override;
    void on_stop() override;

  private:
    // ── msg_id 상수 (msg_registry.toml 기반) ─────────────────────────────
    struct msg_ids
    {
        static constexpr uint32_t LOGIN_REQUEST = 1000;
        static constexpr uint32_t LOGIN_RESPONSE = 1001;
        static constexpr uint32_t LOGOUT_REQUEST = 1002;
        static constexpr uint32_t LOGOUT_RESPONSE = 1003;
        static constexpr uint32_t REFRESH_TOKEN_REQUEST = 1004;
        static constexpr uint32_t REFRESH_TOKEN_RESPONSE = 1005;
    };

    // ── Kafka 핸들러 (MetadataPrefix 기반, kafka_route 시그니처) ──────────
    // current_meta_ 제거 — 메타데이터가 핸들러 파라미터로 직접 전달됨.

    /// Login 요청 처리.
    boost::asio::awaitable<apex::core::Result<void>> on_login(const apex::core::KafkaMessageMeta& meta, uint32_t msg_id,
                                                              const apex::auth_svc::schemas::LoginRequest* req);

    /// Logout 요청 처리.
    boost::asio::awaitable<apex::core::Result<void>> on_logout(const apex::core::KafkaMessageMeta& meta,
                                                               uint32_t msg_id,
                                                               const apex::auth_svc::schemas::LogoutRequest* req);

    /// Refresh Token 요청 처리.
    boost::asio::awaitable<apex::core::Result<void>>
    on_refresh_token(const apex::core::KafkaMessageMeta& meta, uint32_t msg_id,
                     const apex::shared::schemas::RefreshTokenRequest* req);

    /// Kafka 응답 Envelope 빌드 + produce.
    /// EnvelopeBuilder를 사용하여 build() (힙 할당 — Auth 서비스에는 bump 컨텍스트 불필요).
    void send_response(uint32_t msg_id, uint64_t corr_id, uint16_t core_id, uint64_t session_id,
                       std::span<const uint8_t> fbs_payload, const std::string& reply_topic);

    // ── 에러 응답 헬퍼 [D5] ──────────────────────────────────────────────

    /// LoginResponse 에러 응답 빌드 + send.
    boost::asio::awaitable<apex::core::Result<void>> send_login_error(const apex::core::KafkaMessageMeta& meta,
                                                                      uint16_t error);

    /// LogoutResponse 에러 응답 빌드 + send.
    boost::asio::awaitable<apex::core::Result<void>> send_logout_error(const apex::core::KafkaMessageMeta& meta,
                                                                       uint16_t error);

    /// RefreshTokenResponse 에러 응답 빌드 + send.
    boost::asio::awaitable<apex::core::Result<void>> send_refresh_token_error(const apex::core::KafkaMessageMeta& meta,
                                                                              uint16_t error);

    AuthConfig config_;

    // 어댑터 참조 — on_configure()에서 바인딩
    apex::shared::adapters::kafka::KafkaAdapter* kafka_{nullptr};
    apex::shared::adapters::redis::RedisAdapter* redis_{nullptr};
    apex::shared::adapters::pg::PgAdapter* pg_{nullptr};

    // 비즈니스 로직 컴포넌트
    JwtManager jwt_manager_;
    PasswordHasher password_hasher_;
    std::unique_ptr<SessionStore> session_store_; // Redis 바인딩 후 생성
};

} // namespace apex::auth_svc
