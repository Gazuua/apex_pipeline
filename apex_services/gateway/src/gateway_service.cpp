// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

// NOMINMAX must precede any Windows header to prevent min/max macro
// conflicts with std::numeric_limits in FlatBuffers headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <apex/gateway/broadcast_fanout.hpp>
#include <apex/gateway/channel_session_map.hpp>
#include <apex/gateway/gateway_service.hpp>
#include <apex/gateway/jwt_blacklist.hpp>
#include <apex/gateway/jwt_verifier.hpp>
#include <apex/gateway/pubsub_listener.hpp>
#include <apex/gateway/response_dispatcher.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/rate_limit/endpoint_rate_config.hpp>
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <apex/shared/rate_limit/rate_limit_facade.hpp>
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>

#include <apex/core/error_sender.hpp>
#include <apex/core/server.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/gateway/gateway_error.hpp>
#include <flatbuffers/flatbuffers.h>

#include <stdexcept>

namespace apex::gateway
{

// Gateway 프로토콜 규약: 시스템 메시지(AUTHENTICATE, SUBSCRIBE, UNSUBSCRIBE)의
// FlatBuffers 페이로드에서 첫 번째 필드(vtable offset 4)는 항상 primary string
// (JWT 토큰 또는 채널 이름). 스키마 비의존 — Gateway MSA 독립성 유지.
constexpr flatbuffers::voffset_t kPrimaryStringField = 4;

// GatewayGlobals 소멸자 — PubSubListener 정지 포함
GatewayGlobals::~GatewayGlobals()
{
    if (pubsub_listener)
    {
        pubsub_listener->stop();
    }
}

GatewayService::GatewayService(const GatewayConfig& config, const JwtVerifier& jwt_verifier, RouteTablePtr route_table)
    : ServiceBase("gateway")
    , config_(config)
    , jwt_verifier_(&jwt_verifier)
    , route_table_(std::move(route_table))
    , pending_requests_(config_.max_pending_per_core, config_.request_timeout)
{}

GatewayService::~GatewayService() = default;

// ── Phase 1: 어댑터 참조 획득 + per-core 컴포넌트 초기화 ─────────────────
void GatewayService::on_configure(apex::core::ConfigureContext& ctx)
{
    // 어댑터 참조 획득 (Server가 라이프사이클 관리)
    kafka_ = &ctx.server.adapter<apex::shared::adapters::kafka::KafkaAdapter>();
    rl_redis_adapter_ = &ctx.server.adapter<apex::shared::adapters::redis::RedisAdapter>("ratelimit");

    // per-core 컴포넌트 초기화 (어댑터 참조 필요)
    pipeline_ = std::make_unique<GatewayPipeline>(config_, *jwt_verifier_, nullptr, nullptr);
    router_ = std::make_unique<MessageRouter>(*kafka_, route_table_, static_cast<uint16_t>(ctx.core_id),
                                              config_.kafka_response_topic);

    logger_.info("GatewayService on_configure (core_id={})", ctx.core_id);
}

// ── Phase 2: cross-core 와이어링 ─────────────────────────────────────────
void GatewayService::on_wire(apex::core::WireContext& ctx)
{
    // core 0: factory 실행하여 GatewayGlobals 생성 (Server가 소유)
    // core 1+: 이미 생성된 인스턴스 반환 (factory 미실행)
    // on_wire는 단일 스레드(main)에서 순차 호출되므로 동기화 불필요.
    if (ctx.core_id == 0)
    {
        globals_ = &ctx.server.global<GatewayGlobals>([&]() { return create_globals(ctx); });
        // BroadcastFanout에 per-core 채널 맵 바인딩 — globals_가 server.global<T>()에
        // move-store된 후이므로 per_core_channel_maps 주소가 확정됨.
        if (globals_->broadcast_fanout)
        {
            globals_->broadcast_fanout->set_channel_maps(&globals_->per_core_channel_maps);
        }
    }
    else
    {
        globals_ = &ctx.server.global<GatewayGlobals>(
            []() -> GatewayGlobals { throw std::logic_error("GatewayGlobals should be created by core 0"); });
    }

    // PubSubListener 와이어링
    if (globals_ && globals_->pubsub_listener)
    {
        pubsub_listener_ = globals_->pubsub_listener.get();
    }

    // Rate limiter 와이어링
    if (globals_ && ctx.core_id < globals_->per_core_facade.size() && globals_->per_core_facade[ctx.core_id])
    {
        pipeline_->set_rate_limiter(globals_->per_core_facade[ctx.core_id].get());
    }

    // per-core 타임아웃 스윕 스케줄링
    schedule_sweep(ctx);

    logger_.info("GatewayService on_wire (core_id={})", ctx.core_id);
}

// ── Phase 3: 핸들러 등록 ────────────────────────────────────────────────
void GatewayService::on_start()
{
    set_default_handler(&GatewayService::on_default_message);
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayService::on_default_message(apex::core::SessionPtr session, uint32_t msg_id, std::span<const uint8_t> payload)
{
    logger_.debug(session, msg_id, "on_default_message");

    if (msg_id == system_msg_ids::AUTHENTICATE_SESSION)
        co_return handle_authenticate_session(session, payload);

    // SUBSCRIBE/UNSUBSCRIBE: require authentication + IP rate limit (BACKLOG-249).
    // Without this guard, unauthenticated clients can flood SUBSCRIBE to exhaust
    // ChannelSessionMap memory → OOM.
    if (msg_id == system_msg_ids::SUBSCRIBE_CHANNEL || msg_id == system_msg_ids::UNSUBSCRIBE_CHANNEL)
    {
        auto it = auth_states_.find(session->id());
        if (it == auth_states_.end() || !it->second.authenticated)
        {
            logger_.debug(session, msg_id, "system message rejected: not authenticated");
            auto frame = apex::core::ErrorSender::build_error_frame(
                msg_id, apex::core::ErrorCode::ServiceError, "", static_cast<uint16_t>(GatewayError::JwtVerifyFailed));
            (void)session->enqueue_write(std::move(frame));
            co_return apex::core::ok();
        }

        if (pipeline_)
        {
            const auto& ip = session->remote_ip();
            auto ip_result = pipeline_->check_ip_rate_limit(ip);
            if (!ip_result)
            {
                logger_.debug(session, msg_id, "system message rate-limited (ip={})", ip);
                auto frame =
                    apex::core::ErrorSender::build_error_frame(msg_id, apex::core::ErrorCode::ServiceError, "",
                                                               static_cast<uint16_t>(GatewayError::RateLimitedIp));
                (void)session->enqueue_write(std::move(frame));
                co_return apex::core::ok();
            }
        }

        if (msg_id == system_msg_ids::SUBSCRIBE_CHANNEL)
            co_return handle_subscribe_channel(session, payload);
        else
            co_return handle_unsubscribe_channel(session, payload);
    }

    co_return co_await handle_request(std::move(session), msg_id, payload);
}

apex::core::Result<void> GatewayService::handle_authenticate_session(apex::core::SessionPtr session,
                                                                     std::span<const uint8_t> payload)
{
    // Client sends JWT token after login; bind it to the session.
    if (payload.size() < sizeof(flatbuffers::uoffset_t))
    {
        logger_.warn(session, "authenticate: payload too small (size={})", payload.size());
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::AUTHENTICATE_SESSION,
                                                                apex::core::ErrorCode::InvalidMessage);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    flatbuffers::Verifier verifier(payload.data(), payload.size());
    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
    if (!verifier.VerifyTableStart(reinterpret_cast<const uint8_t*>(root)))
    {
        logger_.warn(session, "authenticate: FlatBuffers verify failed");
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::AUTHENTICATE_SESSION,
                                                                apex::core::ErrorCode::FlatBuffersVerifyFailed);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }
    verifier.EndTable();

    auto* token = root->GetPointer<const flatbuffers::String*>(kPrimaryStringField);
    if (!token || !verifier.VerifyString(token) || token->size() == 0)
    {
        logger_.warn(session, "authenticate: empty or invalid token");
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::AUTHENTICATE_SESSION,
                                                                apex::core::ErrorCode::ServiceError, "",
                                                                static_cast<uint16_t>(GatewayError::JwtVerifyFailed));
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    auto& state = auth_states_[session->id()];
    state.token = apex::shared::SecureString(token->str());

    // JWT를 즉시 검증하여 AUTHENTICATE 직후 SUBSCRIBE가 가능하도록 함.
    // 이전에는 첫 번째 일반 메시지(handle_request → pipeline_->process)에서만
    // authenticated가 설정되어, AUTHENTICATE 직후 SUBSCRIBE가 거부되었음.
    if (jwt_verifier_)
    {
        auto claims = jwt_verifier_->verify(token->str());
        if (claims.has_value())
        {
            state.authenticated = true;
            state.user_id = claims->user_id;
            state.jti = claims->jti;
            logger_.info(session, "JWT verified and bound (user_id={})", claims->user_id);
        }
        else
        {
            logger_.warn(session, "JWT bound but verification failed — deferred to pipeline");
        }
    }
    else
    {
        logger_.info(session, "JWT bound (no verifier)");
    }
    return apex::core::ok();
}

apex::core::Result<void> GatewayService::handle_subscribe_channel(apex::core::SessionPtr session,
                                                                  std::span<const uint8_t> payload)
{
    // Client subscribes to a Redis Pub/Sub channel for broadcast delivery.
    // Gateway is channel-name-agnostic (no domain knowledge).
    // per-core 맵만 수정 — cross_core_post 불필요 (세션은 per-core).
    if (!globals_)
    {
        return apex::core::ok();
    }

    if (payload.size() < sizeof(flatbuffers::uoffset_t))
    {
        logger_.warn(session, "subscribe: payload too small (size={})", payload.size());
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::SUBSCRIBE_CHANNEL,
                                                                apex::core::ErrorCode::InvalidMessage);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    flatbuffers::Verifier verifier(payload.data(), payload.size());
    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
    if (!verifier.VerifyTableStart(reinterpret_cast<const uint8_t*>(root)))
    {
        logger_.warn(session, "subscribe: FlatBuffers verify failed");
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::SUBSCRIBE_CHANNEL,
                                                                apex::core::ErrorCode::FlatBuffersVerifyFailed);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }
    verifier.EndTable();

    auto* ch = root->GetPointer<const flatbuffers::String*>(kPrimaryStringField);
    if (!ch || !verifier.VerifyString(ch) || ch->size() == 0)
    {
        logger_.warn(session, "subscribe: empty or invalid channel name");
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::SUBSCRIBE_CHANNEL,
                                                                apex::core::ErrorCode::InvalidMessage);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    auto result = globals_->per_core_channel_maps[core_id()].subscribe(ch->str(), session->id());
    if (!result)
    {
        logger_.warn(session, "subscribe: limit exceeded for '{}'", ch->str());
        auto frame = apex::core::ErrorSender::build_error_frame(
            system_msg_ids::SUBSCRIBE_CHANNEL, apex::core::ErrorCode::ServiceError, "",
            static_cast<uint16_t>(GatewayError::SubscriptionLimitExceeded));
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    if (pubsub_listener_)
    {
        pubsub_listener_->subscribe(ch->str());
    }
    logger_.info(session, "subscribed to '{}'", ch->str());
    return apex::core::ok();
}

apex::core::Result<void> GatewayService::handle_unsubscribe_channel(apex::core::SessionPtr session,
                                                                    std::span<const uint8_t> payload)
{
    // per-core 맵만 수정 — cross_core_post 불필요.
    if (!globals_)
    {
        return apex::core::ok();
    }

    if (payload.size() < sizeof(flatbuffers::uoffset_t))
    {
        logger_.warn(session, "unsubscribe: payload too small (size={})", payload.size());
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::UNSUBSCRIBE_CHANNEL,
                                                                apex::core::ErrorCode::InvalidMessage);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    flatbuffers::Verifier verifier(payload.data(), payload.size());
    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
    if (!verifier.VerifyTableStart(reinterpret_cast<const uint8_t*>(root)))
    {
        logger_.warn(session, "unsubscribe: FlatBuffers verify failed");
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::UNSUBSCRIBE_CHANNEL,
                                                                apex::core::ErrorCode::FlatBuffersVerifyFailed);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }
    verifier.EndTable();

    auto* ch = root->GetPointer<const flatbuffers::String*>(kPrimaryStringField);
    if (!ch || !verifier.VerifyString(ch) || ch->size() == 0)
    {
        logger_.warn(session, "unsubscribe: empty or invalid channel name");
        auto frame = apex::core::ErrorSender::build_error_frame(system_msg_ids::UNSUBSCRIBE_CHANNEL,
                                                                apex::core::ErrorCode::InvalidMessage);
        (void)session->enqueue_write(std::move(frame));
        return apex::core::ok();
    }

    globals_->per_core_channel_maps[core_id()].unsubscribe(ch->str(), session->id());
    logger_.info(session, "unsubscribed from '{}'", ch->str());
    return apex::core::ok();
}

void GatewayService::on_stop()
{
    dispatcher().clear_default_handler();
    // PubSubListener 정지는 core 0에서만 수행.
    // GatewayGlobals 소멸자에서도 stop()을 호출하지만,
    // on_stop 단계에서 명시적으로 정리하는 것이 graceful shutdown에 유리.
    if (core_id() == 0 && globals_ && globals_->pubsub_listener)
    {
        globals_->pubsub_listener->stop();
    }
    auth_states_.clear();
}

// ── 세션 종료 콜백 ──────────────────────────────────────────────────────
void GatewayService::on_session_closed(apex::core::SessionId sid)
{
    logger_.info("session closed sid={}", sid);
    // per-session 인증 상태 제거
    auth_states_.erase(sid);

    // 채널 구독 해제 — per-core 맵이므로 로컬 맵만 수정 (뮤텍스 불필요)
    if (globals_)
    {
        globals_->per_core_channel_maps[core_id()].unsubscribe_all(sid);
    }
}

// ── cross-core 글로벌 객체 생성 (core 0 전용, factory 반환) ──────────────
GatewayGlobals GatewayService::create_globals(apex::core::WireContext& ctx)
{
    auto num_cores = ctx.server.core_count();
    GatewayGlobals g;

    // per-core 채널 구독 맵 초기화
    g.per_core_channel_maps.reserve(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i)
    {
        g.per_core_channel_maps.emplace_back(config_.max_subscriptions_per_session);
    }
    g.engine = &ctx.server.core_engine();
    g.num_cores = num_cores;

    // ── per-core 포인터 수집 ─────────────────────────────────────────
    std::vector<PendingRequestsMap*> pending_maps;
    std::vector<apex::core::SessionManager*> session_mgrs;

    for (uint32_t i = 0; i < num_cores; ++i)
    {
        auto& state = ctx.server.per_core_state(i);
        auto* gw_svc = state.registry.find<GatewayService>();
        if (!gw_svc)
        {
            throw std::logic_error("GatewayService not registered in ServiceRegistry");
        }
        pending_maps.push_back(&gw_svc->pending_requests());
        session_mgrs.push_back(&state.session_mgr);
    }

    // ── ResponseDispatcher ───────────────────────────────────────────
    g.response_dispatcher = std::make_unique<ResponseDispatcher>(ctx.server.core_engine(), std::move(pending_maps),
                                                                 std::move(session_mgrs));

    // Kafka 콜백: this->globals_ 를 캡처. factory 반환 후 on_wire에서
    // globals_가 설정되며, Kafka 메시지 도착 시점에는 유효.
    kafka_->set_message_callback([this](std::string_view /*topic*/, int32_t /*partition*/,
                                        std::span<const uint8_t> /*key*/, std::span<const uint8_t> payload,
                                        int64_t /*timestamp*/) -> apex::core::Result<void> {
        if (!globals_ || !globals_->response_dispatcher)
        {
            return std::unexpected(apex::core::ErrorCode::ServiceError);
        }
        return globals_->response_dispatcher->on_response(payload);
    });

    // ── BroadcastFanout ──────────────────────────────────────────────
    // BroadcastFanout은 GatewayGlobals* 를 참조. create_globals() 반환 후
    // Server::global<T>()이 소유하며, on_wire에서 globals_ 포인터 설정됨.
    // fanout은 런타임에 globals_->per_core_channel_maps를 직접 참조한다.
    std::vector<apex::core::SessionManager*> session_mgrs2;
    for (uint32_t i = 0; i < num_cores; ++i)
    {
        session_mgrs2.push_back(&ctx.server.per_core_state(i).session_mgr);
    }
    g.broadcast_fanout =
        std::make_unique<BroadcastFanout>(ctx.server.core_engine(), num_cores, std::move(session_mgrs2));

    // ── PubSubListener ───────────────────────────────────────────────
    auto* fanout_ptr = g.broadcast_fanout.get();
    PubSubListener::Config pubsub_cfg{
        .host = config_.redis_pubsub_host,
        .port = config_.redis_pubsub_port,
        .password = config_.redis_pubsub_password,
        .initial_channels = config_.global_channels,
    };
    g.pubsub_listener = std::make_unique<PubSubListener>(
        pubsub_cfg, [fanout_ptr](std::string_view channel, std::span<const uint8_t> message) {
            fanout_ptr->fanout(channel, message);
        });
    g.pubsub_listener->start();

    // ── Rate Limiting ────────────────────────────────────────────────
    // Server::add_adapter()가 라이프사이클(init/drain/close)을 관리하므로 수동 init 불필요.
    if (rl_redis_adapter_)
    {
        // EndpointRateConfig 빌드
        apex::shared::rate_limit::EndpointRateConfig ep_config;
        ep_config.default_limit = config_.rate_limit.endpoint.default_limit;
        ep_config.window_size = std::chrono::seconds{config_.rate_limit.endpoint.window_size_seconds};
        for (auto& [msg_id, limit] : config_.rate_limit.endpoint.overrides)
        {
            ep_config.overrides[msg_id] = limit;
        }

        // per-core rate limit 컴포넌트 생성
        g.per_core_ip.resize(num_cores);
        g.per_core_redis_rl.resize(num_cores);
        g.per_core_facade.resize(num_cores);

        for (uint32_t core = 0; core < num_cores; ++core)
        {
            apex::shared::rate_limit::PerIpRateLimiterConfig ip_cfg{
                .total_limit = config_.rate_limit.ip.total_limit,
                .window_size = std::chrono::seconds{config_.rate_limit.ip.window_size_seconds},
                .num_cores = num_cores,
                .max_entries = config_.rate_limit.ip.max_entries,
                .ttl_multiplier = config_.rate_limit.ip.ttl_multiplier,
            };
            g.per_core_ip[core] = std::make_unique<apex::shared::rate_limit::PerIpRateLimiter>(
                ip_cfg, [](auto /*key*/, auto /*window*/) -> uint64_t { return 0; }, [](auto /*key*/) {},
                [](auto /*key*/, auto /*count*/) {});

            apex::shared::rate_limit::RedisRateLimiterConfig redis_rl_config{
                .default_limit = config_.rate_limit.user.default_limit,
                .window_size = std::chrono::seconds{config_.rate_limit.user.window_size_seconds},
            };
            g.per_core_redis_rl[core] = std::make_unique<apex::shared::rate_limit::RedisRateLimiter>(
                redis_rl_config, rl_redis_adapter_->multiplexer(core));

            g.per_core_facade[core] = std::make_unique<apex::shared::rate_limit::RateLimitFacade>(
                *g.per_core_ip[core], *g.per_core_redis_rl[core], ep_config);
        }
        logger_.info("Rate limiting enabled ({} cores)", num_cores);
    }

    return g;
}

// ── per-core 타임아웃 스윕 스케줄링 ──────────────────────────────────────
void GatewayService::schedule_sweep(apex::core::WireContext& ctx)
{
    // 각 코어에서 자기 코어의 pending_requests를 1초마다 스윕.
    // PeriodicTaskScheduler는 per-core이므로 core 스레드에서 실행됨 → lock-free.
    auto* pending = &pending_requests_;
    auto* session_mgr = &ctx.server.per_core_state(ctx.core_id).session_mgr;

    ctx.scheduler.schedule(std::chrono::milliseconds{config_.sweep_interval_ms}, [pending, session_mgr]() {
        pending->sweep_expired([session_mgr](uint64_t /*corr_id*/, const PendingRequestsMap::PendingEntry& entry) {
            auto session = session_mgr->find_session(entry.session_id);
            if (session && session->is_open())
            {
                auto frame = apex::core::ErrorSender::build_error_frame(
                    entry.original_msg_id, apex::core::ErrorCode::ServiceError, "",
                    static_cast<uint16_t>(GatewayError::ServiceTimeout));
                (void)session->enqueue_write(std::move(frame));
            }
        });
    });
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayService::handle_request(apex::core::SessionPtr session, uint32_t msg_id, std::span<const uint8_t> payload)
{
    // 1. Get or create per-session auth state
    auto& state = auth_states_[session->id()];

    // 2. Resolve remote IP (cached at accept time — no syscall)
    const auto& remote_ip = session->remote_ip();

    // 3. Build WireHeader for pipeline + router
    apex::core::WireHeader header;
    header.msg_id = msg_id;
    header.body_size = static_cast<uint32_t>(payload.size());

    // 4. Run gateway pipeline (rate limit + auth)
    auto pipeline_result = co_await pipeline_->process(session, header, state, remote_ip);
    if (!pipeline_result)
    {
        logger_.debug(session, msg_id, "pipeline denied");
        // Pipeline already sent error frame to client — don't propagate to connection_handler
        co_return apex::core::ok();
    }

    // 5. Route to Kafka via MessageRouter
    auto corr_id = router_->generate_corr_id();

    // 6. Register pending request (for response correlation)
    auto pending_result = pending_requests_.insert(corr_id, session->id(), msg_id);
    if (!pending_result)
    {
        logger_.warn(session, msg_id, "pending map full");
        auto frame = apex::core::ErrorSender::build_error_frame(msg_id, apex::core::ErrorCode::ServiceError, "",
                                                                static_cast<uint16_t>(GatewayError::PendingMapFull));
        (void)session->enqueue_write(std::move(frame));
        co_return apex::core::ok();
    }

    // 7. Produce to Kafka
    auto route_result = router_->route(session, header, payload, state.user_id, corr_id);
    if (!route_result)
    {
        logger_.error("handle_request: route failed for msg_id={}", msg_id);
        // Remove pending entry on failure
        (void)pending_requests_.extract(corr_id);
        auto frame = apex::core::ErrorSender::build_error_frame(msg_id, apex::core::ErrorCode::ServiceError, "",
                                                                static_cast<uint16_t>(GatewayError::RouteNotFound));
        (void)session->enqueue_write(std::move(frame));
        co_return apex::core::ok();
    }
    logger_.info("handle_request: msg_id={} routed successfully (corr_id={})", msg_id, corr_id);

    co_return apex::core::ok();
}

} // namespace apex::gateway
