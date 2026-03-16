// NOMINMAX must precede any Windows header to prevent min/max macro
// conflicts with std::numeric_limits in FlatBuffers headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>
#include <apex/gateway/gateway_service.hpp>
#include <apex/gateway/response_dispatcher.hpp>
#include <apex/gateway/route_table.hpp>
#include <apex/gateway/broadcast_fanout.hpp>
#include <apex/gateway/channel_session_map.hpp>
#include <apex/gateway/pubsub_listener.hpp>

#include <apex/core/server.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>
#include <apex/shared/protocols/websocket/websocket_protocol.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/rate_limit/rate_limit_facade.hpp>
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>
#include <apex/shared/rate_limit/endpoint_rate_config.hpp>

#include <boost/asio/steady_timer.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <functional>
#include <memory>

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Apex Gateway starting...");

    // Config path (default: gateway.toml)
    std::string config_path = "gateway.toml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // Config parsing
    auto config_result = apex::gateway::parse_gateway_config(config_path);
    if (!config_result) {
        spdlog::error("Failed to parse config: {}", config_path);
        return EXIT_FAILURE;
    }
    auto& gw_config = *config_result;

    // Build RouteTable from config
    auto route_table_result = apex::gateway::RouteTable::build(gw_config.routes);
    if (!route_table_result) {
        spdlog::error("Failed to build route table");
        return EXIT_FAILURE;
    }
    auto route_table = std::make_shared<const apex::gateway::RouteTable>(
        std::move(*route_table_result));

    // JWT Verifier (shared, immutable after construction)
    auto jwt_verifier = std::make_shared<apex::gateway::JwtVerifier>(gw_config.jwt);

    // Kafka config
    apex::shared::adapters::kafka::KafkaConfig kafka_cfg;
    kafka_cfg.brokers = gw_config.kafka_brokers;
    kafka_cfg.consumer_group = gw_config.kafka_consumer_group;
    kafka_cfg.consume_topics = {gw_config.kafka_response_topic};

    // Redis config (Auth)
    apex::shared::adapters::redis::RedisConfig redis_auth_cfg;
    redis_auth_cfg.host = gw_config.redis_auth_host;
    redis_auth_cfg.port = gw_config.redis_auth_port;
    redis_auth_cfg.password = gw_config.redis_auth_password;

    // --- Channel Session Map (shared, thread-safe) ---
    auto channel_map = std::make_shared<apex::gateway::ChannelSessionMap>(
        gw_config.max_subscriptions_per_session);

    // --- PubSub Listener (created here, started in post_init) ---
    apex::gateway::PubSubListener::Config pubsub_cfg{
        .host = gw_config.redis_pubsub_host,
        .port = gw_config.redis_pubsub_port,
        .password = gw_config.redis_pubsub_password,
        .initial_channels = gw_config.global_channels,
    };

    std::unique_ptr<apex::gateway::BroadcastFanout> broadcast_fanout;
    std::unique_ptr<apex::gateway::PubSubListener> pubsub_listener;
    std::unique_ptr<apex::gateway::ResponseDispatcher> response_dispatcher;

    // Rate limiting — standalone RedisAdapter (separate from Server's adapter registry)
    apex::shared::adapters::redis::RedisConfig redis_rl_cfg;
    redis_rl_cfg.host = gw_config.redis_ratelimit_host;
    redis_rl_cfg.port = gw_config.redis_ratelimit_port;
    redis_rl_cfg.password = gw_config.redis_ratelimit_password;
    auto rl_redis_adapter = std::make_unique<
        apex::shared::adapters::redis::RedisAdapter>(redis_rl_cfg);

    // Per-core rate limit components (created in post_init, kept alive here)
    struct PerCoreRateLimit {
        std::unique_ptr<apex::shared::rate_limit::PerIpRateLimiter> ip;
        std::unique_ptr<apex::shared::rate_limit::RedisRateLimiter> redis;
        std::unique_ptr<apex::shared::rate_limit::RateLimitFacade> facade;
    };
    std::vector<PerCoreRateLimit> per_core_rl;

    auto* channel_map_ptr = channel_map.get();

    // Server setup
    apex::core::Server server({
        .num_cores = gw_config.num_cores,
        .heartbeat_timeout_ticks = 300,
    });

    server
        .listen<apex::shared::protocols::websocket::WebSocketProtocol>(
            gw_config.ws_port);

    if (gw_config.tcp_port > 0) {
        server.listen<apex::shared::protocols::tcp::TcpBinaryProtocol>(
            gw_config.tcp_port);
        spdlog::info("TCP Binary listener on port {}", gw_config.tcp_port);
    }

    server
        .add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(kafka_cfg)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>(redis_auth_cfg);

    // GatewayService per-core factory
    auto gw_config_copy = gw_config;
    server.add_service_factory(
        [gw_config_copy, route_table, jwt_verifier,
         channel_map_ptr,
         &server, &pubsub_listener](apex::core::PerCoreState& state)
            -> std::unique_ptr<apex::core::ServiceBaseInterface> {
            auto& kafka = server.adapter<
                apex::shared::adapters::kafka::KafkaAdapter>();
            apex::gateway::GatewayService::Dependencies deps{
                .kafka = kafka,
                .jwt_verifier = *jwt_verifier,
                .jwt_blacklist = nullptr,
                .route_table = route_table,
                .core_id = state.core_id,
                .channel_map = channel_map_ptr,
                .pubsub_listener = pubsub_listener.get(),
                // rate_limiter = nullptr for now (rate limit tests addressed separately)
            };
            return std::make_unique<apex::gateway::GatewayService>(
                gw_config_copy, std::move(deps));
        });

    // Post-init callback: wire ResponseDispatcher + BroadcastFanout + PubSubListener + Sweep
    server.set_post_init_callback(
        [&response_dispatcher, &broadcast_fanout, &pubsub_listener,
         &pubsub_cfg, channel_map_ptr,
         &rl_redis_adapter, &per_core_rl, gw_config_copy](apex::core::Server& srv) {
            auto num_cores = srv.core_count();

            // Collect per-core pointers
            std::vector<apex::gateway::PendingRequestsMap*> pending_maps;
            std::vector<apex::core::SessionManager*> session_mgrs;

            for (uint32_t i = 0; i < num_cores; ++i) {
                auto& state = srv.per_core_state(i);
                auto* gw_svc = dynamic_cast<apex::gateway::GatewayService*>(
                    state.services[0].get());
                pending_maps.push_back(&gw_svc->pending_requests());
                session_mgrs.push_back(&state.session_mgr);
            }

            // --- ResponseDispatcher ---
            response_dispatcher = std::make_unique<apex::gateway::ResponseDispatcher>(
                srv.core_engine(),
                std::move(pending_maps),
                std::move(session_mgrs));

            auto& kafka = srv.adapter<
                apex::shared::adapters::kafka::KafkaAdapter>();
            kafka.set_message_callback(
                [&rd = *response_dispatcher](
                    std::string_view, int32_t,
                    std::span<const uint8_t>,
                    std::span<const uint8_t> payload,
                    int64_t) -> apex::core::Result<void> {
                    return rd.on_response(payload);
                });

            // --- BroadcastFanout ---
            std::vector<apex::core::SessionManager*> session_mgrs2;
            for (uint32_t i = 0; i < num_cores; ++i) {
                session_mgrs2.push_back(&srv.per_core_state(i).session_mgr);
            }
            broadcast_fanout = std::make_unique<apex::gateway::BroadcastFanout>(
                srv.core_engine(), *channel_map_ptr, std::move(session_mgrs2));

            // --- PubSubListener ---
            auto* fanout_ptr = broadcast_fanout.get();
            pubsub_listener = std::make_unique<apex::gateway::PubSubListener>(
                pubsub_cfg,
                [fanout_ptr](std::string_view channel,
                             std::span<const uint8_t> message) {
                    fanout_ptr->fanout(channel, message);
                });
            pubsub_listener->start();

            // --- Pending Requests Timeout Sweep ---
            // Re-collect pending maps (previous was moved)
            std::vector<apex::gateway::PendingRequestsMap*> sweep_maps;
            std::vector<apex::core::SessionManager*> sweep_mgrs;
            for (uint32_t i = 0; i < num_cores; ++i) {
                auto& state = srv.per_core_state(i);
                auto* gw_svc = dynamic_cast<apex::gateway::GatewayService*>(
                    state.services[0].get());
                sweep_maps.push_back(&gw_svc->pending_requests());
                sweep_mgrs.push_back(&state.session_mgr);
            }

            struct SweepState {
                std::vector<apex::gateway::PendingRequestsMap*> pending;
                std::vector<apex::core::SessionManager*> sessions;
                apex::core::CoreEngine* engine;
                std::shared_ptr<boost::asio::steady_timer> timer;
            };
            auto ss = std::make_shared<SweepState>();
            ss->pending = std::move(sweep_maps);
            ss->sessions = std::move(sweep_mgrs);
            ss->engine = &srv.core_engine();
            ss->timer = std::make_shared<boost::asio::steady_timer>(
                srv.core_engine().io_context(0));

            std::function<void(boost::system::error_code)> on_sweep;
            on_sweep = [ss, on_sweep](boost::system::error_code ec) {
                if (ec) return;

                // Sweep each core's pending map on its own io_context
                for (size_t core = 0; core < ss->pending.size(); ++core) {
                    auto* pending = ss->pending[core];
                    auto* mgr = ss->sessions[core];
                    auto core_id = static_cast<uint32_t>(core);

                    boost::asio::post(
                        ss->engine->io_context(core_id),
                        [pending, mgr]() {
                            pending->sweep_expired(
                                [mgr](uint64_t,
                                      const apex::gateway::PendingRequestsMap::PendingEntry& entry) {
                                    auto session = mgr->find_session(entry.session_id);
                                    if (session && session->is_open()) {
                                        auto frame = apex::core::ErrorSender::build_error_frame(
                                            entry.original_msg_id,
                                            apex::core::ErrorCode::ServiceTimeout);
                                        (void)session->enqueue_write(std::move(frame));
                                    }
                                });
                        });
                }

                // Re-arm timer (1 second interval)
                ss->timer->expires_after(std::chrono::seconds{1});
                ss->timer->async_wait(on_sweep);
            };

            ss->timer->expires_after(std::chrono::seconds{1});
            ss->timer->async_wait(on_sweep);

            // --- Rate Limiting ---
            // Initialize standalone Redis adapter for rate limiting
            rl_redis_adapter->do_init(srv.core_engine());

            // Build EndpointRateConfig from GatewayConfig
            apex::shared::rate_limit::EndpointRateConfig ep_config;
            ep_config.default_limit = gw_config_copy.rate_limit.endpoint.default_limit;
            ep_config.window_size = std::chrono::seconds{
                gw_config_copy.rate_limit.endpoint.window_size_seconds};
            for (auto& [msg_id, limit] : gw_config_copy.rate_limit.endpoint.overrides) {
                ep_config.overrides[msg_id] = limit;
            }

            // Create per-core rate limit components
            per_core_rl.resize(num_cores);
            for (uint32_t core = 0; core < num_cores; ++core) {
                // PerIpRateLimiter (no-op timer callbacks — TTL cleanup not needed for short-lived E2E)
                apex::shared::rate_limit::PerIpRateLimiterConfig ip_cfg{
                    .total_limit = gw_config_copy.rate_limit.ip.total_limit,
                    .window_size = std::chrono::seconds{
                        gw_config_copy.rate_limit.ip.window_size_seconds},
                    .num_cores = num_cores,
                    .max_entries = gw_config_copy.rate_limit.ip.max_entries,
                    .ttl_multiplier = gw_config_copy.rate_limit.ip.ttl_multiplier,
                };
                per_core_rl[core].ip = std::make_unique<
                    apex::shared::rate_limit::PerIpRateLimiter>(
                    ip_cfg,
                    [](auto, auto) -> uint64_t { return 0; },  // no-op schedule
                    [](auto) {},                                 // no-op cancel
                    [](auto, auto) {}                           // no-op reschedule
                );

                // RedisRateLimiter (needs multiplexer from standalone adapter)
                apex::shared::rate_limit::RedisRateLimiterConfig redis_rl_config{
                    .default_limit = gw_config_copy.rate_limit.user.default_limit,
                    .window_size = std::chrono::seconds{
                        gw_config_copy.rate_limit.user.window_size_seconds},
                };
                per_core_rl[core].redis = std::make_unique<
                    apex::shared::rate_limit::RedisRateLimiter>(
                    redis_rl_config,
                    rl_redis_adapter->multiplexer(core));

                // RateLimitFacade
                per_core_rl[core].facade = std::make_unique<
                    apex::shared::rate_limit::RateLimitFacade>(
                    *per_core_rl[core].ip,
                    *per_core_rl[core].redis,
                    ep_config);

                // Wire to GatewayService
                auto* gw_svc = dynamic_cast<apex::gateway::GatewayService*>(
                    srv.per_core_state(core).services[0].get());
                gw_svc->set_rate_limiter(per_core_rl[core].facade.get());
            }

            spdlog::info("Rate limiting enabled ({} cores)", num_cores);
        });

    server.run();

    // Cleanup
    if (pubsub_listener) pubsub_listener->stop();

    spdlog::info("Apex Gateway stopped.");
    return EXIT_SUCCESS;
}
