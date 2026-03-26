// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/chat_svc/chat_service.hpp>

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/config_utils.hpp>
#include <toml++/toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{

/// TOML 설정 파싱 결과
struct ParsedConfig
{
    apex::chat_svc::ChatService::Config chat;
    apex::shared::adapters::kafka::KafkaConfig kafka;
    apex::shared::adapters::redis::RedisConfig redis_data;
    apex::shared::adapters::redis::RedisConfig redis_pubsub;
    apex::shared::adapters::pg::PgAdapterConfig pg;
};

/// Resolve config file path: CLI arg or default "chat_svc.toml" next to executable.
std::string resolve_config_path(int argc, char* argv[])
{
    if (argc >= 2)
    {
        return argv[1];
    }

    // Default: look for chat_svc.toml next to the executable
    auto exe_dir = std::filesystem::path(argv[0]).parent_path();
    auto default_path = exe_dir / "chat_svc.toml";
    if (std::filesystem::exists(default_path))
    {
        return default_path.string();
    }

    // Fallback: current working directory
    return "chat_svc.toml";
}

ParsedConfig parse_config(const std::string& path)
{
    auto tbl = toml::parse_file(path);
    ParsedConfig cfg;

    // [chat]
    if (auto chat = tbl["chat"]; chat)
    {
        cfg.chat.request_topic = chat["request_topic"].value_or(std::string{"chat.requests"});
        cfg.chat.response_topic = chat["response_topic"].value_or(std::string{"chat.responses"});
        cfg.chat.persist_topic = chat["persist_topic"].value_or(std::string{"chat.messages.persist"});
        cfg.chat.max_room_members = static_cast<uint32_t>(chat["max_room_members"].value_or(int64_t{100}));
        cfg.chat.max_message_length = static_cast<uint32_t>(chat["max_message_length"].value_or(int64_t{2000}));
        cfg.chat.history_page_size = static_cast<uint32_t>(chat["history_page_size"].value_or(int64_t{50}));
        cfg.chat.max_room_name_length = static_cast<size_t>(chat["max_room_name_length"].value_or(int64_t{100}));
        cfg.chat.max_list_rooms_limit = static_cast<uint32_t>(chat["max_list_rooms_limit"].value_or(int64_t{100}));
    }

    // [kafka]
    if (auto kafka = tbl["kafka"]; kafka)
    {
        cfg.kafka.brokers = kafka["brokers"].value_or(std::string{"localhost:9092"});
        cfg.kafka.consumer_group = kafka["consumer_group"].value_or(std::string{"chat-svc"});
    }
    // Ensure request topic is in consume_topics
    cfg.kafka.consume_topics = {cfg.chat.request_topic};

    // [redis_data] -- Redis for room membership/online status
    if (auto redis_data = tbl["redis_data"]; redis_data)
    {
        cfg.redis_data.host = redis_data["host"].value_or(std::string{"localhost"});
        cfg.redis_data.port = static_cast<uint16_t>(redis_data["port"].value_or(int64_t{6379}));
        cfg.redis_data.db = static_cast<uint32_t>(redis_data["db"].value_or(int64_t{0}));
        cfg.redis_data.password = apex::shared::expand_env(redis_data["password"].value_or(std::string{}));
    }

    // [redis_pubsub] -- Redis for pub/sub broadcast
    if (auto redis_pubsub = tbl["redis_pubsub"]; redis_pubsub)
    {
        cfg.redis_pubsub.host = redis_pubsub["host"].value_or(std::string{"localhost"});
        cfg.redis_pubsub.port = static_cast<uint16_t>(redis_pubsub["port"].value_or(int64_t{6379}));
        cfg.redis_pubsub.db = static_cast<uint32_t>(redis_pubsub["db"].value_or(int64_t{0}));
        cfg.redis_pubsub.password = apex::shared::expand_env(redis_pubsub["password"].value_or(std::string{}));
    }

    // [pg]
    if (auto pg = tbl["pg"]; pg)
    {
        cfg.pg.connection_string = apex::shared::expand_env(
            pg["connection_string"].value_or(std::string{"host=localhost port=6432 dbname=apex user=apex"}));
        cfg.pg.pool_size_per_core = static_cast<size_t>(pg["pool_size_per_core"].value_or(int64_t{2}));
    }

    return cfg;
}

} // anonymous namespace

int main(int argc, char* argv[])
{
    // --- 1. Config parsing ---
    auto config_path = resolve_config_path(argc, argv);

    // 로깅 초기화 (TOML의 [logging] 섹션 사용)
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);

    apex::core::ScopedLogger logger{"Main", apex::core::ScopedLogger::NO_CORE, "app"};
    logger.info("Chat Service starting...");
    logger.info("Loading config: {}", config_path);

    ParsedConfig parsed;
    try
    {
        parsed = parse_config(config_path);
    }
    catch (const toml::parse_error& e)
    {
        logger.error("Failed to parse config '{}': {}", config_path, e.what());
        return EXIT_FAILURE;
    }

    // --- 2. Server 구성 ---
    apex::core::ServerConfig server_config;
    server_config.num_cores = 1;
    server_config.metrics = app_config.metrics;
    server_config.admin = app_config.admin;
    apex::core::Server server(server_config);

    server.add_adapter<apex::shared::adapters::kafka::KafkaAdapter>(parsed.kafka)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>("data", parsed.redis_data)
        .add_adapter<apex::shared::adapters::redis::RedisAdapter>("pubsub", parsed.redis_pubsub)
        .add_adapter<apex::shared::adapters::pg::PgAdapter>(parsed.pg)
        .add_service<apex::chat_svc::ChatService>(std::move(parsed.chat));

    // Kafka auto-wiring [D2]: kafka_route() 등록만 하면 코어가 KafkaDispatchBridge를 자동 배선.
    // post_init_callback 수동 와이어링 제거됨.

    // --- 3. Server 실행 (블로킹) ---
    logger.info("Running. Press Ctrl+C to stop.");
    server.run();

    logger.info("Chat Service stopped.");
    return EXIT_SUCCESS;
}
