// NOMINMAX must precede any Windows header to prevent min/max macro
// conflicts with std::numeric_limits in FlatBuffers headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>
#include <apex/gateway/gateway_service.hpp>
#include <apex/gateway/route_table.hpp>

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
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
    // ── Config 파싱 ──────────────────────────────────────────────────
    std::string config_path = "gateway.toml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // 로깅 초기화 (TOML의 [logging] 섹션 사용)
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);

    spdlog::info("Apex Gateway starting...");

    auto config_result = apex::gateway::parse_gateway_config(config_path);
    if (!config_result) {
        spdlog::error("Failed to parse config: {}", config_path);
        return EXIT_FAILURE;
    }
    auto& gw_config = *config_result;

    // ── RouteTable 빌드 ──────────────────────────────────────────────
    auto route_table_result = apex::gateway::RouteTable::build(gw_config.routes);
    if (!route_table_result) {
        spdlog::error("Failed to build route table");
        return EXIT_FAILURE;
    }
    auto route_table = std::make_shared<const apex::gateway::RouteTable>(
        std::move(*route_table_result));

    // ── Immutable shared objects ─────────────────────────────────────
    auto jwt_verifier = std::make_shared<apex::gateway::JwtVerifier>(gw_config.jwt);

    // ── Adapter configs ──────────────────────────────────────────────
    apex::shared::adapters::kafka::KafkaConfig kafka_cfg;
    kafka_cfg.brokers = gw_config.kafka_brokers;
    kafka_cfg.consumer_group = gw_config.kafka_consumer_group;
    kafka_cfg.consume_topics = {gw_config.kafka_response_topic};

    apex::shared::adapters::redis::RedisConfig redis_auth_cfg;
    redis_auth_cfg.host = gw_config.redis_auth_host;
    redis_auth_cfg.port = gw_config.redis_auth_port;
    redis_auth_cfg.password = gw_config.redis_auth_password;

    // ── Rate limit standalone Redis adapter ──────────────────────────
    // Server의 어댑터 레지스트리와 분리된 독립 어댑터.
    // GatewayService가 on_wire()에서 직접 초기화.
    apex::shared::adapters::redis::RedisConfig redis_rl_cfg;
    redis_rl_cfg.host = gw_config.redis_ratelimit_host;
    redis_rl_cfg.port = gw_config.redis_ratelimit_port;
    redis_rl_cfg.password = gw_config.redis_ratelimit_password;
    auto rl_redis_adapter = std::make_unique<
        apex::shared::adapters::redis::RedisAdapter>(redis_rl_cfg);

    // ── Server 설정 ──────────────────────────────────────────────────
    apex::core::Server server({
        .num_cores = gw_config.num_cores,
        .heartbeat_timeout_ticks = gw_config.heartbeat_timeout_ticks,
    });

    server
        .listen<apex::shared::protocols::websocket::WebSocketProtocol>(
            gw_config.ws_port);

    if (gw_config.tcp_port > 0) {
        server.listen<apex::shared::protocols::tcp::TcpBinaryProtocol>(
            gw_config.tcp_port);
        spdlog::info("TCP Binary listener on port {}", gw_config.tcp_port);
    }

    // 어댑터 등록 (role 기반 다중 등록)
    server
        .add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(kafka_cfg)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>("auth", redis_auth_cfg);

    // ── GatewayService per-core 팩토리 ───────────────────────────────
    // GatewayService의 라이프사이클 훅이 모든 와이어링을 수행:
    //   on_configure — Kafka 어댑터 참조 + per-core 컴포넌트 초기화
    //   on_wire      — ResponseDispatcher/BroadcastFanout/PubSubListener 생성 (core 0)
    //                  + RateLimiter/Sweep 와이어링 (전 코어)
    //   on_start     — 기본 핸들러 등록
    //   on_session_closed — auth_states/채널 구독 정리
    auto gw_config_copy = gw_config;
    auto* rl_adapter_ptr = rl_redis_adapter.get();

    server.add_service_factory(
        [gw_config_copy, route_table, jwt_verifier,
         rl_adapter_ptr](apex::core::PerCoreState& /*state*/)
            -> std::unique_ptr<apex::core::ServiceBaseInterface> {
            return std::make_unique<apex::gateway::GatewayService>(
                gw_config_copy, *jwt_verifier,
                route_table, rl_adapter_ptr);
        });

    // post_init_callback 불필요 — GatewayService 라이프사이클이 모든 와이어링 수행.

    server.run();

    spdlog::info("Apex Gateway stopped.");
    return EXIT_SUCCESS;
}
