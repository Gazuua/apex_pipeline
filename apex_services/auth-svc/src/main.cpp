#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/auth_service.hpp>

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
#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <chrono>
#include <cstdlib>
#include <string>

namespace
{

/// Parse auth_svc.toml and populate AuthConfig + adapter configs.
struct ParsedConfig
{
    apex::auth_svc::AuthConfig auth;
    apex::shared::adapters::kafka::KafkaConfig kafka;
    apex::shared::adapters::redis::RedisConfig redis;
    apex::shared::adapters::pg::PgAdapterConfig pg;
};

ParsedConfig parse_config(const std::string& path)
{
    auto tbl = toml::parse_file(path);
    ParsedConfig cfg;

    // [jwt]
    if (auto jwt = tbl["jwt"]; jwt)
    {
        cfg.auth.jwt_private_key_path = jwt["private_key_file"].value_or(std::string{"keys/auth_rs256.pem"});
        cfg.auth.jwt_public_key_path = jwt["public_key_file"].value_or(std::string{"keys/auth_rs256_pub.pem"});
        cfg.auth.jwt_issuer = jwt["issuer"].value_or(std::string{"apex-auth"});
        cfg.auth.access_token_ttl = std::chrono::seconds{jwt["access_token_ttl_sec"].value_or(int64_t{900})};
        cfg.auth.refresh_token_ttl = std::chrono::seconds{jwt["refresh_token_ttl_sec"].value_or(int64_t{604800})};
    }

    // [kafka]
    if (auto kafka = tbl["kafka"]; kafka)
    {
        cfg.kafka.brokers = kafka["brokers"].value_or(std::string{"localhost:9092"});
        cfg.kafka.consumer_group = kafka["consumer_group"].value_or(std::string{"auth-svc"});

        cfg.auth.request_topic = kafka["request_topic"].value_or(std::string{"auth.requests"});
        cfg.auth.response_topic = kafka["response_topic"].value_or(std::string{"auth.responses"});

        // Subscribe to request topic
        cfg.kafka.consume_topics = {cfg.auth.request_topic};
    }

    // [redis]
    if (auto redis = tbl["redis"]; redis)
    {
        cfg.redis.host = redis["host"].value_or(std::string{"localhost"});
        cfg.redis.port = static_cast<uint16_t>(redis["port"].value_or(int64_t{6380}));
        cfg.redis.password = redis["password"].value_or(std::string{});
        cfg.redis.db = static_cast<uint32_t>(redis["db"].value_or(int64_t{0}));
    }

    // [pg]
    if (auto pg = tbl["pg"]; pg)
    {
        cfg.pg.connection_string = pg["connection_string"].value_or(
            std::string{"host=localhost port=5432 dbname=apex_db user=apex_user password=apex_pass"});
        cfg.pg.pool_size_per_core = static_cast<size_t>(pg["pool_size_per_core"].value_or(int64_t{2}));
    }

    // [bcrypt]
    if (auto bcrypt = tbl["bcrypt"]; bcrypt)
    {
        cfg.auth.bcrypt_work_factor = static_cast<uint32_t>(bcrypt["work_factor"].value_or(int64_t{12}));
    }

    // [session]
    if (auto session = tbl["session"]; session)
    {
        cfg.auth.session_ttl = std::chrono::seconds{session["ttl_sec"].value_or(int64_t{86400})};
    }

    return cfg;
}

} // anonymous namespace

int main(int argc, char* argv[])
{
    // --- 1. Config path (default: auth_svc.toml) ---
    std::string config_path = "auth_svc.toml";
    if (argc > 1)
    {
        config_path = argv[1];
    }

    // 로깅 초기화 (TOML의 [logging] 섹션 사용)
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);

    spdlog::info("Auth Service starting...");

    // --- 2. Parse TOML config ---
    ParsedConfig parsed;
    try
    {
        parsed = parse_config(config_path);
    }
    catch (const toml::parse_error& e)
    {
        spdlog::error("Failed to parse config '{}': {}", config_path, e.what());
        return EXIT_FAILURE;
    }

    spdlog::info("Config loaded from '{}'", config_path);

    // --- 3. Server 구성 (CoreEngine 내장) ---
    apex::core::ServerConfig server_config;
    server_config.num_cores = 1;
    apex::core::Server server(server_config);

    // --- 4. 어댑터 등록 ---
    server.add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(parsed.kafka)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>(parsed.redis)
        .add_adapter<apex::shared::adapters::pg::PgAdapter>(parsed.pg);

    // --- 5. AuthService 등록 ---
    auto auth_config = parsed.auth;
    server.add_service<apex::auth_svc::AuthService>(std::move(auth_config));

    // --- 6. Server 실행 (블로킹) ---
    // Kafka auto-wiring [D2]: kafka_route() 등록만 하면 코어가 KafkaDispatchBridge를 자동 배선.
    // post_init_callback 수동 와이어링 제거됨.
    spdlog::info("[AuthService] Running. Press Ctrl+C to stop.");
    server.run();

    spdlog::info("Auth Service stopped.");
    return EXIT_SUCCESS;
}
