#include <apex/chat_svc/chat_service.hpp>

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

/// TOML 설정 파싱 결과
struct ParsedConfig {
    apex::chat_svc::ChatService::Config chat;
    apex::shared::adapters::kafka::KafkaConfig kafka;
    apex::shared::adapters::redis::RedisConfig redis_data;
    apex::shared::adapters::redis::RedisConfig redis_pubsub;
    apex::shared::adapters::pg::PgAdapterConfig pg;
};

/// Resolve config file path: CLI arg or default "chat_svc.toml" next to executable.
std::string resolve_config_path(int argc, char* argv[]) {
    if (argc >= 2) {
        return argv[1];
    }

    // Default: look for chat_svc.toml next to the executable
    auto exe_dir = std::filesystem::path(argv[0]).parent_path();
    auto default_path = exe_dir / "chat_svc.toml";
    if (std::filesystem::exists(default_path)) {
        return default_path.string();
    }

    // Fallback: current working directory
    return "chat_svc.toml";
}

ParsedConfig parse_config(const std::string& path) {
    auto tbl = toml::parse_file(path);
    ParsedConfig cfg;

    // [chat]
    if (auto chat = tbl["chat"]; chat) {
        cfg.chat.request_topic      = chat["request_topic"]
            .value_or(std::string{"chat.requests"});
        cfg.chat.response_topic     = chat["response_topic"]
            .value_or(std::string{"chat.responses"});
        cfg.chat.persist_topic      = chat["persist_topic"]
            .value_or(std::string{"chat.messages.persist"});
        cfg.chat.max_room_members   = static_cast<uint32_t>(
            chat["max_room_members"].value_or(int64_t{100}));
        cfg.chat.max_message_length = static_cast<uint32_t>(
            chat["max_message_length"].value_or(int64_t{2000}));
        cfg.chat.history_page_size  = static_cast<uint32_t>(
            chat["history_page_size"].value_or(int64_t{50}));
    }

    // [kafka]
    if (auto kafka = tbl["kafka"]; kafka) {
        cfg.kafka.brokers        = kafka["brokers"]
            .value_or(std::string{"localhost:9092"});
        cfg.kafka.consumer_group = kafka["consumer_group"]
            .value_or(std::string{"chat-svc"});
    }
    // Ensure request topic is in consume_topics
    cfg.kafka.consume_topics = {cfg.chat.request_topic};

    // [redis_data] -- Redis for room membership/online status
    if (auto redis_data = tbl["redis_data"]; redis_data) {
        cfg.redis_data.host     = redis_data["host"]
            .value_or(std::string{"localhost"});
        cfg.redis_data.port     = static_cast<uint16_t>(
            redis_data["port"].value_or(int64_t{6379}));
        cfg.redis_data.db       = static_cast<uint32_t>(
            redis_data["db"].value_or(int64_t{0}));
        cfg.redis_data.password = redis_data["password"]
            .value_or(std::string{});
    }

    // [redis_pubsub] -- Redis for pub/sub broadcast
    if (auto redis_pubsub = tbl["redis_pubsub"]; redis_pubsub) {
        cfg.redis_pubsub.host     = redis_pubsub["host"]
            .value_or(std::string{"localhost"});
        cfg.redis_pubsub.port     = static_cast<uint16_t>(
            redis_pubsub["port"].value_or(int64_t{6379}));
        cfg.redis_pubsub.db       = static_cast<uint32_t>(
            redis_pubsub["db"].value_or(int64_t{0}));
        cfg.redis_pubsub.password = redis_pubsub["password"]
            .value_or(std::string{});
    }

    // [pg]
    if (auto pg = tbl["pg"]; pg) {
        cfg.pg.connection_string  = pg["connection_string"]
            .value_or(std::string{"host=localhost port=6432 dbname=apex user=apex"});
        cfg.pg.pool_size_per_core = static_cast<size_t>(
            pg["pool_size_per_core"].value_or(int64_t{2}));
    }

    return cfg;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    // --- 1. Config parsing ---
    auto config_path = resolve_config_path(argc, argv);

    // 로깅 초기화 (TOML의 [logging] 섹션 사용)
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);

    spdlog::info("Chat Service starting...");
    spdlog::info("[ChatService] Loading config: {}", config_path);

    ParsedConfig parsed;
    try {
        parsed = parse_config(config_path);
    } catch (const toml::parse_error& e) {
        spdlog::error("[ChatService] Failed to parse config '{}': {}", config_path, e.what());
        return EXIT_FAILURE;
    }

    // --- 2. Server 구성 ---
    // request_topic을 move 전에 캡처 (post_init 콜백에서 사용)
    auto request_topic = parsed.chat.request_topic;

    apex::core::Server server({.num_cores = 1});

    server
        .add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(parsed.kafka)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>("data", parsed.redis_data)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>("pubsub", parsed.redis_pubsub)
        .add_adapter<apex::shared::adapters::pg::PgAdapter>(parsed.pg)
        .add_service<apex::chat_svc::ChatService>(std::move(parsed.chat));

    // --- 3. Kafka consumer -> KafkaDispatchBridge 와이어링 ---
    // post_init_callback에서 서비스의 kafka_handler_map()에 접근하여
    // KafkaDispatchBridge를 생성하고 Kafka consumer 콜백으로 연결.
    server.set_post_init_callback(
        [request_topic](apex::core::Server& srv) {
            auto& state = srv.per_core_state(0);

            // ChatService는 첫 번째(유일한) 서비스
            auto* chat_svc = dynamic_cast<apex::chat_svc::ChatService*>(
                state.services[0].get());

            // KafkaDispatchBridge 생성 -- 핸들러 맵 참조
            // shared_ptr로 관리하여 Kafka 콜백 람다에서 캡처
            auto bridge = std::make_shared<
                apex::shared::protocols::kafka::KafkaDispatchBridge>(
                chat_svc->kafka_handler_map());

            // Kafka consumer 콜백 설정
            // Kafka 콜백은 동기이므로 co_spawn으로 코루틴 브릿지.
            // payload를 복사하여 코루틴 수명 안전성 보장.
            auto& kafka = srv.adapter<
                apex::shared::adapters::kafka::KafkaAdapter>();
            auto& engine = srv.core_engine();
            kafka.set_message_callback(
                [bridge, request_topic, &engine](
                    std::string_view topic, int32_t /*partition*/,
                    std::span<const uint8_t> /*key*/,
                    std::span<const uint8_t> payload,
                    int64_t /*offset*/) -> apex::core::Result<void> {
                    if (topic != request_topic) {
                        return apex::core::ok();  // 다른 토픽 무시
                    }

                    // Kafka 콜백의 payload는 콜백 반환 후 무효 -> 복사 필수
                    auto payload_copy = std::vector<uint8_t>(
                        payload.begin(), payload.end());

                    // co_spawn으로 코루틴 실행 -- bridge->dispatch가 파싱+핸들러 호출 수행
                    boost::asio::co_spawn(engine.io_context(0),
                        [bridge, data = std::move(payload_copy)]()
                            -> boost::asio::awaitable<void> {
                            auto result = co_await bridge->dispatch(
                                std::span<const uint8_t>(data));
                            if (!result.has_value()) {
                                spdlog::warn("[ChatService] Kafka dispatch failed: {}",
                                    static_cast<int>(result.error()));
                            }
                        },
                        boost::asio::detached);

                    return apex::core::ok();
                });

            // PG connection warm-up
            auto& pg = srv.adapter<apex::shared::adapters::pg::PgAdapter>();
            boost::asio::co_spawn(engine.io_context(0),
                [&pg]() -> boost::asio::awaitable<void> {
                    auto r = co_await pg.query("SELECT 1");
                    if (r.has_value()) {
                        spdlog::info("[ChatService] PG connection warm-up OK");
                    } else {
                        spdlog::warn("[ChatService] PG warm-up failed (will retry on first query)");
                    }
                },
                boost::asio::detached);
            std::this_thread::sleep_for(std::chrono::seconds{1});

            spdlog::info("[ChatService] KafkaDispatchBridge wired for topic: {}",
                         request_topic);
        });

    // --- 4. Server 실행 (블로킹) ---
    spdlog::info("[ChatService] Running. Press Ctrl+C to stop.");
    server.run();

    spdlog::info("Chat Service stopped.");
    return EXIT_SUCCESS;
}
