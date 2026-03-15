#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>
#include <apex/gateway/gateway_service.hpp>

#include <apex/core/server.hpp>
#include <apex/shared/protocols/websocket/websocket_protocol.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <spdlog/spdlog.h>

#include <cstdlib>

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
            gw_config.ws_port)
        .add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(kafka_cfg)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>(redis_auth_cfg)
        .add_service<apex::gateway::GatewayService>(gw_config)
        .run();

    spdlog::info("Apex Gateway stopped.");
    return EXIT_SUCCESS;
}
