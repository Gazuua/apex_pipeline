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

#include <apex/core/server.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>
#include <apex/shared/protocols/websocket/websocket_protocol.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <spdlog/spdlog.h>

#include <cstdlib>
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

    // GatewayService per-core factory — captures shared resources by value (shared_ptr)
    // and injects per-core state (core_id) from PerCoreState.
    auto gw_config_copy = gw_config;
    server.add_service_factory(
        [gw_config_copy, route_table, jwt_verifier,
         &server](apex::core::PerCoreState& state)
            -> std::unique_ptr<apex::core::ServiceBaseInterface> {
            auto& kafka = server.adapter<
                apex::shared::adapters::kafka::KafkaAdapter>();
            apex::gateway::GatewayService::Dependencies deps{
                .kafka = kafka,
                .jwt_verifier = *jwt_verifier,
                .jwt_blacklist = nullptr,  // TODO: wire JwtBlacklist with RedisMultiplexer
                .route_table = route_table,
                .core_id = state.core_id,
            };
            return std::make_unique<apex::gateway::GatewayService>(
                gw_config_copy, std::move(deps));
        });

    // ResponseDispatcher wiring — after services + adapters are initialized.
    // Collects per-core PendingRequestsMap and SessionManager pointers,
    // creates ResponseDispatcher, and sets Kafka consumer callback.
    std::unique_ptr<apex::gateway::ResponseDispatcher> response_dispatcher;

    server.set_post_init_callback(
        [&response_dispatcher](apex::core::Server& srv) {
            auto num_cores = srv.core_count();

            // Collect per-core pointers from services
            std::vector<apex::gateway::PendingRequestsMap*> pending_maps;
            std::vector<apex::core::SessionManager*> session_mgrs;

            for (uint32_t i = 0; i < num_cores; ++i) {
                auto& state = srv.per_core_state(i);
                // First service on each core is GatewayService
                auto* gw_svc = dynamic_cast<apex::gateway::GatewayService*>(
                    state.services[0].get());
                pending_maps.push_back(&gw_svc->pending_requests());
                session_mgrs.push_back(&state.session_mgr);
            }

            response_dispatcher = std::make_unique<apex::gateway::ResponseDispatcher>(
                srv.core_engine(),
                std::move(pending_maps),
                std::move(session_mgrs));

            // Set Kafka consumer callback to route responses
            auto& kafka = srv.adapter<
                apex::shared::adapters::kafka::KafkaAdapter>();
            kafka.set_message_callback(
                [&rd = *response_dispatcher](
                    std::string_view /*topic*/, int32_t /*partition*/,
                    std::span<const uint8_t> /*key*/,
                    std::span<const uint8_t> payload,
                    int64_t /*offset*/) -> apex::core::Result<void> {
                    return rd.on_response(payload);
                });
        });

    server.run();

    spdlog::info("Apex Gateway stopped.");
    return EXIT_SUCCESS;
}
