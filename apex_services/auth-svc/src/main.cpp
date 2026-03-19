#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/auth_service.hpp>
#include <apex/auth_svc/password_hasher.hpp>

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

/// Parse auth_svc.toml and populate AuthConfig + adapter configs.
struct ParsedConfig {
    apex::auth_svc::AuthConfig auth;
    apex::shared::adapters::kafka::KafkaConfig kafka;
    apex::shared::adapters::redis::RedisConfig redis;
    apex::shared::adapters::pg::PgAdapterConfig pg;
};

ParsedConfig parse_config(const std::string& path) {
    auto tbl = toml::parse_file(path);
    ParsedConfig cfg;

    // [jwt]
    if (auto jwt = tbl["jwt"]; jwt) {
        cfg.auth.jwt_private_key_path = jwt["private_key_file"]
            .value_or(std::string{"keys/auth_rs256.pem"});
        cfg.auth.jwt_public_key_path = jwt["public_key_file"]
            .value_or(std::string{"keys/auth_rs256_pub.pem"});
        cfg.auth.jwt_issuer = jwt["issuer"]
            .value_or(std::string{"apex-auth"});
        cfg.auth.access_token_ttl = std::chrono::seconds{
            jwt["access_token_ttl_sec"].value_or(int64_t{900})};
        cfg.auth.refresh_token_ttl = std::chrono::seconds{
            jwt["refresh_token_ttl_sec"].value_or(int64_t{604800})};
    }

    // [kafka]
    if (auto kafka = tbl["kafka"]; kafka) {
        cfg.kafka.brokers = kafka["brokers"]
            .value_or(std::string{"localhost:9092"});
        cfg.kafka.consumer_group = kafka["consumer_group"]
            .value_or(std::string{"auth-svc"});

        cfg.auth.request_topic = kafka["request_topic"]
            .value_or(std::string{"auth.requests"});
        cfg.auth.response_topic = kafka["response_topic"]
            .value_or(std::string{"auth.responses"});

        // Subscribe to request topic
        cfg.kafka.consume_topics = {cfg.auth.request_topic};
    }

    // [redis]
    if (auto redis = tbl["redis"]; redis) {
        cfg.redis.host = redis["host"]
            .value_or(std::string{"localhost"});
        cfg.redis.port = static_cast<uint16_t>(
            redis["port"].value_or(int64_t{6380}));
        cfg.redis.password = redis["password"]
            .value_or(std::string{});
        cfg.redis.db = static_cast<uint32_t>(
            redis["db"].value_or(int64_t{0}));
    }

    // [pg]
    if (auto pg = tbl["pg"]; pg) {
        cfg.pg.connection_string = pg["connection_string"]
            .value_or(std::string{"host=localhost port=5432 dbname=apex_db user=apex_user password=apex_pass"});
        cfg.pg.pool_size_per_core = static_cast<size_t>(
            pg["pool_size_per_core"].value_or(int64_t{2}));
    }

    // [bcrypt]
    if (auto bcrypt = tbl["bcrypt"]; bcrypt) {
        cfg.auth.bcrypt_work_factor = static_cast<uint32_t>(
            bcrypt["work_factor"].value_or(int64_t{12}));
    }

    // [session]
    if (auto session = tbl["session"]; session) {
        cfg.auth.session_ttl = std::chrono::seconds{
            session["ttl_sec"].value_or(int64_t{86400})};
    }

    return cfg;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    // --- 1. Config path (default: auth_svc.toml) ---
    std::string config_path = "auth_svc.toml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // 로깅 초기화 (TOML의 [logging] 섹션 사용)
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);

    spdlog::info("Auth Service starting...");

    // --- 2. Parse TOML config ---
    ParsedConfig parsed;
    try {
        parsed = parse_config(config_path);
    } catch (const toml::parse_error& e) {
        spdlog::error("Failed to parse config '{}': {}", config_path, e.what());
        return EXIT_FAILURE;
    }

    spdlog::info("Config loaded from '{}'", config_path);

    // --- 3. Server 구성 (CoreEngine 내장) ---
    apex::core::Server server({.num_cores = 1});

    // --- 4. 어댑터 등록 ---
    server
        .add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(parsed.kafka)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>(parsed.redis)
        .add_adapter<apex::shared::adapters::pg::PgAdapter>(parsed.pg);

    // --- 5. AuthService 등록 ---
    auto auth_config = parsed.auth;
    server.add_service<apex::auth_svc::AuthService>(std::move(auth_config));

    // --- 6. Kafka consumer → KafkaDispatchBridge 와이어링 ---
    // post_init_callback에서 서비스의 kafka_handler_map()에 접근하여
    // KafkaDispatchBridge를 생성하고 Kafka consumer 콜백으로 연결.
    auto bcrypt_work_factor = parsed.auth.bcrypt_work_factor;
    server.set_post_init_callback(
        [bcrypt_work_factor, &parsed](apex::core::Server& srv) {
            auto& state = srv.per_core_state(0);

            // AuthService는 첫 번째(유일한) 서비스
            auto* auth_svc = dynamic_cast<apex::auth_svc::AuthService*>(
                state.services[0].get());

            // KafkaDispatchBridge 생성 — 핸들러 맵 참조
            // shared_ptr로 관리하여 Kafka 콜백 람다에서 캡처
            auto bridge = std::make_shared<
                apex::shared::protocols::kafka::KafkaDispatchBridge>(
                auth_svc->kafka_handler_map());

            // Kafka consumer 콜백 설정
            // Kafka 콜백은 동기이므로 co_spawn으로 코루틴 브릿지.
            // payload를 복사하여 코루틴 수명 안전성 보장.
            auto& kafka = srv.adapter<
                apex::shared::adapters::kafka::KafkaAdapter>();
            auto& engine = srv.core_engine();
            kafka.set_message_callback(
                [bridge, request_topic = parsed.auth.request_topic, &engine](
                    std::string_view topic, int32_t /*partition*/,
                    std::span<const uint8_t> /*key*/,
                    std::span<const uint8_t> payload,
                    int64_t /*offset*/) -> apex::core::Result<void> {
                    if (topic != request_topic) {
                        return apex::core::ok();  // 다른 토픽 무시
                    }

                    // Kafka 콜백의 payload는 콜백 반환 후 무효 → 복사 필수
                    auto payload_copy = std::vector<uint8_t>(
                        payload.begin(), payload.end());

                    // co_spawn으로 코루틴 실행 — bridge->dispatch가 파싱+핸들러 호출 수행
                    boost::asio::co_spawn(engine.io_context(0),
                        [bridge, data = std::move(payload_copy)]()
                            -> boost::asio::awaitable<void> {
                            auto result = co_await bridge->dispatch(
                                std::span<const uint8_t>(data));
                            if (!result.has_value()) {
                                spdlog::warn("[AuthService] Kafka dispatch failed: {}",
                                    static_cast<int>(result.error()));
                            }
                        },
                        boost::asio::detached);

                    return apex::core::ok();
                });

            // --- 테스트 사용자 시드 (E2E) ---
            apex::auth_svc::PasswordHasher hasher(bcrypt_work_factor);
            auto hash = hasher.hash("password123");
            if (!hash.empty()) {
                auto& pg = srv.adapter<apex::shared::adapters::pg::PgAdapter>();
                std::array<std::string, 1> params = {hash};
                boost::asio::co_spawn(engine.io_context(0),
                    [&pg, params]() -> boost::asio::awaitable<void> {
                        auto result = co_await pg.execute(
                            "UPDATE users SET password_hash = $1 WHERE password_hash = 'PENDING'",
                            params);
                        if (result.has_value()) {
                            spdlog::info("[AuthService] Seeded test user passwords");
                        } else {
                            spdlog::warn("[AuthService] Password seed failed (may already be set)");
                        }
                    },
                    boost::asio::detached);
                std::this_thread::sleep_for(std::chrono::seconds{1});
            }
        });

    // --- 7. Server 실행 (블로킹) ---
    spdlog::info("[AuthService] Running. Press Ctrl+C to stop.");
    server.run();

    spdlog::info("Auth Service stopped.");
    return EXIT_SUCCESS;
}
