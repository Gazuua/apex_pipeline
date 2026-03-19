#pragma once

#include <apex/core/configure_context.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_context.hpp>
#include <apex/gateway/channel_session_map.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_pipeline.hpp>
#include <apex/gateway/message_router.hpp>
#include <apex/gateway/pending_requests.hpp>

#include <spdlog/spdlog.h>

#include <memory>
#include <unordered_map>

// Forward declarations to avoid pulling in heavy headers
namespace apex::shared::adapters::kafka
{
class KafkaAdapter;
}
namespace apex::shared::adapters::redis
{
class RedisAdapter;
}
namespace apex::shared::rate_limit
{
class RateLimitFacade;
class PerIpRateLimiter;
class RedisRateLimiter;
} // namespace apex::shared::rate_limit

namespace apex::gateway
{

// Forward declarations
class JwtVerifier;
class JwtBlacklist;
class PubSubListener;
class ResponseDispatcher;
class BroadcastFanout;

/// Gateway 시스템 메시지 ID 상수.
/// 프레임워크가 직접 처리하는 시스템 메시지용이며, 서비스 라우팅을 거치지 않는다.
struct system_msg_ids
{
    static constexpr uint16_t AUTHENTICATE_SESSION = 3;
    static constexpr uint16_t SUBSCRIBE_CHANNEL = 4;
    static constexpr uint16_t UNSUBSCRIBE_CHANNEL = 5;
};

/// cross-core 글로벌 객체 — Server.global<T>()가 소유, 서비스는 raw pointer로 참조.
/// core 0의 on_wire()에서 factory를 통해 생성, Server 소멸 시 자동 파괴.
struct GatewayGlobals
{
    std::unique_ptr<ResponseDispatcher> response_dispatcher;
    std::unique_ptr<BroadcastFanout> broadcast_fanout;
    std::unique_ptr<PubSubListener> pubsub_listener;

    // per-core 채널 구독 맵 (인덱스 = core_id). 뮤텍스 불필요 — 각 코어 스레드에서만 접근.
    std::vector<ChannelSessionMap> per_core_channel_maps;

    // 브로드캐스트용 (PubSubListener → 모든 코어에 post)
    apex::core::CoreEngine* engine{nullptr};
    uint32_t num_cores{0};

    // per-core rate limit 컴포넌트 (인덱스 = core_id)
    std::vector<std::unique_ptr<apex::shared::rate_limit::PerIpRateLimiter>> per_core_ip;
    std::vector<std::unique_ptr<apex::shared::rate_limit::RedisRateLimiter>> per_core_redis_rl;
    std::vector<std::unique_ptr<apex::shared::rate_limit::RateLimitFacade>> per_core_facade;

    GatewayGlobals() = default;
    GatewayGlobals(GatewayGlobals&&) noexcept = default;
    GatewayGlobals& operator=(GatewayGlobals&&) noexcept = default;
    ~GatewayGlobals();
};

/// Gateway service — generic proxy that routes all client messages
/// through GatewayPipeline (auth + rate limit) then MessageRouter (Kafka produce).
///
/// 라이프사이클:
///   on_configure — 어댑터 참조 획득 (Kafka), per-core 컴포넌트 초기화
///   on_wire      — cross-core 글로벌 객체 생성 (core 0), 스윕 스케줄링
///   on_start     — 기본 핸들러 등록
///   on_session_closed — per-session 리소스 정리
///
/// Gateway core roles:
/// 1. Client WireHeader -> GatewayPipeline (auth/rate) -> MessageRouter (Kafka produce)
/// 2. Kafka response -> WireHeader conversion + client delivery (ResponseDispatcher)
/// 3. Redis Pub/Sub -> broadcast delivery
class GatewayService : public apex::core::ServiceBase<GatewayService>
{
  public:
    GatewayService(const GatewayConfig& config, const JwtVerifier& jwt_verifier, RouteTablePtr route_table,
                   apex::shared::adapters::redis::RedisAdapter* rl_redis_adapter);
    ~GatewayService();

    // ── ServiceBase 라이프사이클 훅 ─────────────────────────────────────────

    /// Phase 1: 어댑터 참조 획득 (Kafka), per-core 컴포넌트 초기화.
    void on_configure(apex::core::ConfigureContext& ctx) override;

    /// Phase 2: cross-core 글로벌 객체 생성 (core 0 only) + 와이어링.
    void on_wire(apex::core::WireContext& ctx) override;

    /// Phase 3: 기본 핸들러 등록.
    void on_start() override;
    void on_stop() override;

    /// 세션 종료 시 per-session 리소스 정리.
    void on_session_closed(apex::core::SessionId sid) override;

    /// Access per-core pending requests map (for ResponseDispatcher wiring).
    [[nodiscard]] PendingRequestsMap& pending_requests() noexcept
    {
        return pending_requests_;
    }

  private:
    /// Default handler: pipeline check -> Kafka produce via MessageRouter.
    boost::asio::awaitable<apex::core::Result<void>> handle_request(apex::core::SessionPtr session, uint32_t msg_id,
                                                                    std::span<const uint8_t> payload);

    /// cross-core 글로벌 객체 생성 (core 0의 on_wire에서 factory로 1회 호출).
    /// server.global<GatewayGlobals>(factory) 패턴용 — GatewayGlobals를 값으로 반환.
    GatewayGlobals create_globals(apex::core::WireContext& ctx);

    /// per-core 스윕 스케줄링 (각 코어의 on_wire에서 호출).
    void schedule_sweep(apex::core::WireContext& ctx);

    GatewayConfig config_;
    std::shared_ptr<spdlog::logger> logger_;

    // on_configure에서 바인딩 — 생성자에서는 미설정
    apex::shared::adapters::kafka::KafkaAdapter* kafka_{nullptr};

    // 생성자에서 전달받는 immutable 의존성
    const JwtVerifier* jwt_verifier_{nullptr};
    RouteTablePtr route_table_;

    // Per-core components (on_configure 이후 초기화)
    std::unique_ptr<GatewayPipeline> pipeline_;
    std::unique_ptr<MessageRouter> router_;
    PendingRequestsMap pending_requests_;

    // Per-session auth state (session_id -> AuthState)
    std::unordered_map<apex::core::SessionId, AuthState> auth_states_;

    PubSubListener* pubsub_listener_{nullptr};

    // cross-core 글로벌 — Server.global<T>()가 소유, raw pointer로 참조
    GatewayGlobals* globals_{nullptr};

    // Rate limit standalone Redis adapter (main에서 전달, 글로벌 생성 시 사용)
    apex::shared::adapters::redis::RedisAdapter* rl_redis_adapter_{nullptr};
};

} // namespace apex::gateway
