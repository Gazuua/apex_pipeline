#pragma once

#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_pipeline.hpp>
#include <apex/gateway/message_router.hpp>
#include <apex/gateway/pending_requests.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/session.hpp>

#include <spdlog/spdlog.h>

#include <memory>
#include <unordered_map>

// Forward declarations to avoid pulling in heavy headers
namespace apex::shared::adapters::kafka { class KafkaAdapter; }
namespace apex::shared::rate_limit { class RateLimitFacade; }

namespace apex::gateway {

// Forward declarations
class JwtVerifier;
class JwtBlacklist;
class ChannelSessionMap;
class PubSubListener;

/// Gateway service — generic proxy that routes all client messages
/// through GatewayPipeline (auth + rate limit) then MessageRouter (Kafka produce).
///
/// Gateway core roles:
/// 1. Client WireHeader -> GatewayPipeline (auth/rate) -> MessageRouter (Kafka produce)
/// 2. Kafka response -> WireHeader conversion + client delivery (ResponseDispatcher)
/// 3. Redis Pub/Sub -> broadcast delivery
class GatewayService
    : public apex::core::ServiceBase<GatewayService> {
public:
    struct Dependencies {
        apex::shared::adapters::kafka::KafkaAdapter& kafka;
        const JwtVerifier& jwt_verifier;
        JwtBlacklist* jwt_blacklist = nullptr;  // nullable
        RouteTablePtr route_table;
        uint32_t core_id = 0;
        ChannelSessionMap* channel_map = nullptr;  // for Pub/Sub subscription
        PubSubListener* pubsub_listener = nullptr; // for dynamic channel subscribe
        apex::shared::rate_limit::RateLimitFacade* rate_limiter = nullptr;
    };

    GatewayService(const GatewayConfig& config, Dependencies deps);
    ~GatewayService();

    /// ServiceBase hook — registers default handler that routes all
    /// unmatched msg_ids through pipeline + router.
    void on_start() override;
    void on_stop() override;

    /// Access per-core pending requests map (for ResponseDispatcher wiring).
    [[nodiscard]] PendingRequestsMap& pending_requests() noexcept {
        return pending_requests_;
    }

    /// Wire rate limiter after construction (post_init_callback).
    void set_rate_limiter(
        apex::shared::rate_limit::RateLimitFacade* limiter) noexcept {
        pipeline_.set_rate_limiter(limiter);
    }

    /// Wire PubSubListener after construction (post_init_callback).
    /// PubSubListener is created in post_init, after services are constructed.
    void set_pubsub_listener(PubSubListener* listener) noexcept {
        pubsub_listener_ = listener;
    }

private:
    /// Default handler: pipeline check -> Kafka produce via MessageRouter.
    boost::asio::awaitable<apex::core::Result<void>>
    handle_request(apex::core::SessionPtr session,
                   uint32_t msg_id,
                   std::span<const uint8_t> payload);

    GatewayConfig config_;
    std::shared_ptr<spdlog::logger> logger_;

    // Per-core components
    GatewayPipeline pipeline_;
    MessageRouter router_;
    PendingRequestsMap pending_requests_;

    // Per-session auth state (session_id -> AuthState)
    std::unordered_map<apex::core::SessionId, AuthState> auth_states_;

    // Shared (not per-core) — nullable, set via Dependencies
    ChannelSessionMap* channel_map_ = nullptr;
    PubSubListener* pubsub_listener_ = nullptr;
    uint32_t core_id_ = 0;
};

} // namespace apex::gateway
