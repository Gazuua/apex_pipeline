// NOMINMAX must precede any Windows header to prevent min/max macro
// conflicts with std::numeric_limits in FlatBuffers headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <apex/gateway/gateway_service.hpp>
#include <apex/gateway/jwt_verifier.hpp>
#include <apex/gateway/jwt_blacklist.hpp>
#include <apex/gateway/channel_session_map.hpp>
#include <apex/gateway/pubsub_listener.hpp>
#include <apex/gateway/response_dispatcher.hpp>
#include <apex/gateway/broadcast_fanout.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/rate_limit/rate_limit_facade.hpp>
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>
#include <apex/shared/rate_limit/endpoint_rate_config.hpp>

#include <apex/core/server.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/wire_header.hpp>
#include <flatbuffers/flatbuffers.h>

namespace apex::gateway {

// GatewayGlobals 소멸자 — PubSubListener 정지 포함
GatewayGlobals::~GatewayGlobals() {
    if (pubsub_listener) {
        pubsub_listener->stop();
    }
}

GatewayService::GatewayService(const GatewayConfig& config,
                               const JwtVerifier& jwt_verifier,
                               JwtBlacklist* jwt_blacklist,
                               RouteTablePtr route_table,
                               ChannelSessionMap* channel_map,
                               apex::shared::adapters::redis::RedisAdapter* rl_redis_adapter)
    : ServiceBase("gateway")
    , config_(config)
    , logger_(spdlog::default_logger()->clone("gateway"))
    , jwt_verifier_(&jwt_verifier)
    , jwt_blacklist_(jwt_blacklist)
    , route_table_(std::move(route_table))
    , pending_requests_(config_.max_pending_per_core, config_.request_timeout)
    , channel_map_(channel_map)
    , rl_redis_adapter_(rl_redis_adapter)
{
    logger_->info("GatewayService created (routes={})", config_.routes.size());
}

GatewayService::~GatewayService() = default;

// ── Phase 1: 어댑터 참조 획득 + per-core 컴포넌트 초기화 ─────────────────
void GatewayService::on_configure(apex::core::ConfigureContext& ctx) {
    // Kafka 어댑터 참조 획득
    kafka_ = &ctx.server.adapter<apex::shared::adapters::kafka::KafkaAdapter>();

    // per-core 컴포넌트 초기화 (어댑터 참조 필요)
    pipeline_ = std::make_unique<GatewayPipeline>(
        config_, *jwt_verifier_, jwt_blacklist_, nullptr);
    router_ = std::make_unique<MessageRouter>(
        *kafka_, route_table_,
        static_cast<uint16_t>(ctx.core_id),
        config_.kafka_response_topic);

    logger_->info("GatewayService on_configure (core_id={})", ctx.core_id);
}

// ── Phase 2: cross-core 와이어링 ─────────────────────────────────────────
void GatewayService::on_wire(apex::core::WireContext& ctx) {
    // core 0에서 글로벌 객체 생성 (1회)
    if (ctx.core_id == 0) {
        create_globals(ctx);
    }

    // 모든 코어: globals_ 참조로 per-core 와이어링 수행
    // Note: on_wire는 단일 스레드(main)에서 순차 호출되므로
    // core 0 완료 후 core 1이 호출됨. globals_ 접근 안전.
    auto& core0_state = ctx.server.per_core_state(0);
    auto* core0_svc = dynamic_cast<GatewayService*>(
        core0_state.services[0].get());
    globals_ = core0_svc->globals_;

    // PubSubListener 와이어링
    if (globals_ && globals_->pubsub_listener) {
        pubsub_listener_ = globals_->pubsub_listener.get();
    }

    // Rate limiter 와이어링
    if (globals_ && ctx.core_id < globals_->per_core_facade.size()
        && globals_->per_core_facade[ctx.core_id]) {
        pipeline_->set_rate_limiter(globals_->per_core_facade[ctx.core_id].get());
    }

    // per-core 타임아웃 스윕 스케줄링
    schedule_sweep(ctx);

    logger_->info("GatewayService on_wire (core_id={})", ctx.core_id);
}

// ── Phase 3: 핸들러 등록 ────────────────────────────────────────────────
void GatewayService::on_start() {
    // Default handler — routes all messages.
    // System messages (e.g., AuthenticateSession) are handled inline;
    // service messages go through pipeline + Kafka routing.
    dispatcher().set_default_handler(
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {

            // System message: AuthenticateSession (msg_id=3)
            // Client sends JWT token after login; bind it to the session.
            if (msg_id == 3) {
                if (!payload.empty()) {
                    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
                    if (root) {
                        auto* token = root->GetPointer<const flatbuffers::String*>(4);
                        if (token && token->size() > 0) {
                            auto& state = auth_states_[session->id()];
                            state.token = token->str();
                            logger_->info("JWT bound to session {}", session->id());
                        }
                    }
                }
                co_return apex::core::ok();
            }

            // System message: SubscribeChannel (msg_id=4)
            // Client subscribes to a Redis Pub/Sub channel for broadcast delivery.
            // Gateway is channel-name-agnostic (no domain knowledge).
            if (msg_id == 4) {
                if (channel_map_ && !payload.empty()) {
                    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
                    if (root) {
                        auto* ch = root->GetPointer<const flatbuffers::String*>(4);
                        if (ch && ch->size() > 0) {
                            auto result = channel_map_->subscribe(
                                ch->str(), session->id(), core_id());
                            if (result) {
                                if (pubsub_listener_) {
                                    pubsub_listener_->subscribe(ch->str());
                                }
                                logger_->info("Session {} subscribed to '{}'",
                                              session->id(), ch->str());
                            }
                        }
                    }
                }
                co_return apex::core::ok();
            }

            // System message: UnsubscribeChannel (msg_id=5)
            if (msg_id == 5) {
                if (channel_map_ && !payload.empty()) {
                    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
                    if (root) {
                        auto* ch = root->GetPointer<const flatbuffers::String*>(4);
                        if (ch && ch->size() > 0) {
                            channel_map_->unsubscribe(ch->str(), session->id());
                            logger_->info("Session {} unsubscribed from '{}'",
                                          session->id(), ch->str());
                        }
                    }
                }
                co_return apex::core::ok();
            }

            // Service messages — pipeline + Kafka routing
            co_return co_await handle_request(std::move(session), msg_id, payload);
        });
}

void GatewayService::on_stop() {
    dispatcher().clear_default_handler();
    auth_states_.clear();
}

// ── 세션 종료 콜백 ──────────────────────────────────────────────────────
void GatewayService::on_session_closed(apex::core::SessionId sid) {
    // per-session 인증 상태 제거
    auth_states_.erase(sid);

    // 채널 구독 해제 (ChannelSessionMap은 thread-safe)
    if (channel_map_) {
        channel_map_->unsubscribe_all(sid);
    }
}

// ── cross-core 글로벌 객체 생성 (core 0 전용) ────────────────────────────
void GatewayService::create_globals(apex::core::WireContext& ctx) {
    auto num_cores = ctx.server.core_count();
    globals_ = std::make_shared<GatewayGlobals>();

    // ── per-core 포인터 수집 ─────────────────────────────────────────
    std::vector<PendingRequestsMap*> pending_maps;
    std::vector<apex::core::SessionManager*> session_mgrs;

    for (uint32_t i = 0; i < num_cores; ++i) {
        auto& state = ctx.server.per_core_state(i);
        auto* gw_svc = dynamic_cast<GatewayService*>(
            state.services[0].get());
        pending_maps.push_back(&gw_svc->pending_requests());
        session_mgrs.push_back(&state.session_mgr);
    }

    // ── ResponseDispatcher ───────────────────────────────────────────
    globals_->response_dispatcher = std::make_unique<ResponseDispatcher>(
        ctx.server.core_engine(),
        std::move(pending_maps),
        std::move(session_mgrs));

    kafka_->set_message_callback(
        [&rd = *globals_->response_dispatcher](
            std::string_view, int32_t,
            std::span<const uint8_t>,
            std::span<const uint8_t> payload,
            int64_t) -> apex::core::Result<void> {
            return rd.on_response(payload);
        });

    // ── BroadcastFanout ──────────────────────────────────────────────
    std::vector<apex::core::SessionManager*> session_mgrs2;
    for (uint32_t i = 0; i < num_cores; ++i) {
        session_mgrs2.push_back(&ctx.server.per_core_state(i).session_mgr);
    }
    globals_->broadcast_fanout = std::make_unique<BroadcastFanout>(
        ctx.server.core_engine(), *channel_map_, std::move(session_mgrs2));

    // ── PubSubListener ───────────────────────────────────────────────
    auto* fanout_ptr = globals_->broadcast_fanout.get();
    PubSubListener::Config pubsub_cfg{
        .host = config_.redis_pubsub_host,
        .port = config_.redis_pubsub_port,
        .password = config_.redis_pubsub_password,
        .initial_channels = config_.global_channels,
    };
    globals_->pubsub_listener = std::make_unique<PubSubListener>(
        pubsub_cfg,
        [fanout_ptr](std::string_view channel,
                     std::span<const uint8_t> message) {
            fanout_ptr->fanout(channel, message);
        });
    globals_->pubsub_listener->start();

    // ── Rate Limiting ────────────────────────────────────────────────
    if (rl_redis_adapter_) {
        rl_redis_adapter_->do_init(ctx.server.core_engine());

        // EndpointRateConfig 빌드
        apex::shared::rate_limit::EndpointRateConfig ep_config;
        ep_config.default_limit = config_.rate_limit.endpoint.default_limit;
        ep_config.window_size = std::chrono::seconds{
            config_.rate_limit.endpoint.window_size_seconds};
        for (auto& [msg_id, limit] : config_.rate_limit.endpoint.overrides) {
            ep_config.overrides[msg_id] = limit;
        }

        // per-core rate limit 컴포넌트 생성
        globals_->per_core_ip.resize(num_cores);
        globals_->per_core_redis_rl.resize(num_cores);
        globals_->per_core_facade.resize(num_cores);

        for (uint32_t core = 0; core < num_cores; ++core) {
            apex::shared::rate_limit::PerIpRateLimiterConfig ip_cfg{
                .total_limit = config_.rate_limit.ip.total_limit,
                .window_size = std::chrono::seconds{
                    config_.rate_limit.ip.window_size_seconds},
                .num_cores = num_cores,
                .max_entries = config_.rate_limit.ip.max_entries,
                .ttl_multiplier = config_.rate_limit.ip.ttl_multiplier,
            };
            globals_->per_core_ip[core] = std::make_unique<
                apex::shared::rate_limit::PerIpRateLimiter>(
                ip_cfg,
                [](auto, auto) -> uint64_t { return 0; },
                [](auto) {},
                [](auto, auto) {}
            );

            apex::shared::rate_limit::RedisRateLimiterConfig redis_rl_config{
                .default_limit = config_.rate_limit.user.default_limit,
                .window_size = std::chrono::seconds{
                    config_.rate_limit.user.window_size_seconds},
            };
            globals_->per_core_redis_rl[core] = std::make_unique<
                apex::shared::rate_limit::RedisRateLimiter>(
                redis_rl_config,
                rl_redis_adapter_->multiplexer(core));

            globals_->per_core_facade[core] = std::make_unique<
                apex::shared::rate_limit::RateLimitFacade>(
                *globals_->per_core_ip[core],
                *globals_->per_core_redis_rl[core],
                ep_config);
        }
        spdlog::info("Rate limiting enabled ({} cores)", num_cores);
    }
}

// ── per-core 타임아웃 스윕 스케줄링 ──────────────────────────────────────
void GatewayService::schedule_sweep(apex::core::WireContext& ctx) {
    // 각 코어에서 자기 코어의 pending_requests를 1초마다 스윕.
    // PeriodicTaskScheduler는 per-core이므로 core 스레드에서 실행됨 → lock-free.
    auto* pending = &pending_requests_;
    auto* session_mgr = &ctx.server.per_core_state(ctx.core_id).session_mgr;

    ctx.scheduler.schedule(std::chrono::milliseconds{1000},
        [pending, session_mgr]() {
            pending->sweep_expired(
                [session_mgr](uint64_t,
                    const PendingRequestsMap::PendingEntry& entry) {
                    auto session = session_mgr->find_session(entry.session_id);
                    if (session && session->is_open()) {
                        auto frame = apex::core::ErrorSender::build_error_frame(
                            entry.original_msg_id,
                            apex::core::ErrorCode::ServiceTimeout);
                        (void)session->enqueue_write(std::move(frame));
                    }
                });
        });
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayService::handle_request(apex::core::SessionPtr session,
                                uint32_t msg_id,
                                std::span<const uint8_t> payload) {
    // 1. Get or create per-session auth state
    auto& state = auth_states_[session->id()];

    // 2. Resolve remote IP from socket
    std::string remote_ip;
    {
        boost::system::error_code ec;
        auto ep = session->socket().remote_endpoint(ec);
        if (!ec) {
            remote_ip = ep.address().to_string();
        } else {
            remote_ip = "unknown";
        }
    }

    // 3. Build WireHeader for pipeline + router
    apex::core::WireHeader header;
    header.msg_id = msg_id;
    header.body_size = static_cast<uint32_t>(payload.size());

    // 4. Run gateway pipeline (rate limit + auth)
    auto pipeline_result = co_await pipeline_->process(session, header, state, remote_ip);
    if (!pipeline_result) {
        logger_->error("handle_request: pipeline failed for msg_id={}, error={}",
                       msg_id, static_cast<int>(pipeline_result.error()));
        co_return std::unexpected(pipeline_result.error());
    }

    // 5. Route to Kafka via MessageRouter
    auto corr_id = router_->generate_corr_id();

    // 6. Register pending request (for response correlation)
    auto pending_result = pending_requests_.insert(
        corr_id, session->id(), msg_id);
    if (!pending_result) {
        co_return std::unexpected(pending_result.error());
    }

    // 7. Produce to Kafka
    auto route_result = router_->route(
        session, header, payload, state.user_id, corr_id);
    if (!route_result) {
        logger_->error("handle_request: route failed for msg_id={}, error={}",
                       msg_id, static_cast<int>(route_result.error()));
        // Remove pending entry on failure
        (void)pending_requests_.extract(corr_id);
        co_return std::unexpected(route_result.error());
    }
    logger_->info("handle_request: msg_id={} routed successfully (corr_id={})",
                  msg_id, corr_id);

    co_return apex::core::ok();
}

} // namespace apex::gateway
