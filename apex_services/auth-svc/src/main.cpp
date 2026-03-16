#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/auth_service.hpp>

#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
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
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Auth Service starting...");

    // --- 1. Config path (default: auth_svc.toml) ---
    std::string config_path = "auth_svc.toml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // --- 2. Parse TOML config ---
    ParsedConfig parsed;
    try {
        parsed = parse_config(config_path);
    } catch (const toml::parse_error& e) {
        spdlog::error("Failed to parse config '{}': {}", config_path, e.what());
        return EXIT_FAILURE;
    }

    spdlog::info("Config loaded from '{}'", config_path);

    // --- 3. io_context (for co_spawn coroutine execution) ---
    boost::asio::io_context io_ctx;

    // --- 4. Adapters ---
    apex::shared::adapters::kafka::KafkaAdapter kafka(parsed.kafka);
    apex::shared::adapters::redis::RedisAdapter redis(parsed.redis);
    apex::shared::adapters::pg::PgAdapter pg(parsed.pg);

    // --- 5. AuthService (executor only -- no io_context ownership) ---
    apex::auth_svc::AuthService auth(
        std::move(parsed.auth),
        io_ctx.get_executor(),
        kafka, redis, pg);

    // Register handlers to MessageDispatcher + set Kafka consumer callback
    auth.start();

    // --- 6. Signal handling (graceful shutdown) ---
    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& /*ec*/, int sig) {
        spdlog::info("Signal {} received, shutting down...", sig);
        auth.stop();
        io_ctx.stop();
    });

    // --- 7. io_context thread (runs co_spawned coroutines) ---
    std::jthread io_thread([&io_ctx] {
        spdlog::info("[AuthService] io_context thread started");
        io_ctx.run();
        spdlog::info("[AuthService] io_context thread stopped");
    });

    // --- 8. Main thread: Kafka consumer poll loop ---
    // KafkaAdapter's consumer runs via Asio event loop (start_consuming).
    // In standalone mode (no CoreEngine), the main thread blocks on
    // io_context.run() after the io_thread joins on shutdown.
    //
    // NOTE: Standalone adapter initialization (without CoreEngine) will be
    // addressed when the standalone service bootstrap infrastructure is
    // finalized. Currently, adapters require CoreEngine::init() which is
    // provided by Server::run(). For standalone services, a lightweight
    // CoreEngine stub or direct Consumer/Producer management is needed.
    spdlog::info("[AuthService] Running. Press Ctrl+C to stop.");

    // Block main thread until io_context stops (signal-triggered).
    // The io_thread runs the event loop; main thread waits for it to finish.
    io_thread.join();

    // --- 9. Cleanup ---
    spdlog::info("Auth Service stopped.");
    return EXIT_SUCCESS;
}
