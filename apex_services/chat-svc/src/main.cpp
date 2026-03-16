#include <apex/chat_svc/chat_service.hpp>

#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/adapters/kafka/kafka_consumer.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

/// Global stop flag for graceful shutdown.
std::atomic<bool> g_stop_requested{false};

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

} // anonymous namespace

int main(int argc, char* argv[]) {
    spdlog::info("Chat Service starting...");

    // ----------------------------------------------------------------
    // 1. TOML config loading
    // ----------------------------------------------------------------
    auto config_path = resolve_config_path(argc, argv);
    spdlog::info("[ChatService] Loading config: {}", config_path);

    toml::table tbl;
    try {
        tbl = toml::parse_file(config_path);
    } catch (const toml::parse_error& e) {
        spdlog::error("[ChatService] Failed to parse config '{}': {}", config_path, e.what());
        return EXIT_FAILURE;
    }

    // ChatService::Config
    apex::chat_svc::ChatService::Config chat_config;
    if (auto chat = tbl["chat"]; chat) {
        chat_config.request_topic      = chat["request_topic"]
            .value_or(std::string{"chat.requests"});
        chat_config.response_topic     = chat["response_topic"]
            .value_or(std::string{"chat.responses"});
        chat_config.persist_topic      = chat["persist_topic"]
            .value_or(std::string{"chat.messages.persist"});
        chat_config.max_room_members   = static_cast<uint32_t>(
            chat["max_room_members"].value_or(int64_t{100}));
        chat_config.max_message_length = static_cast<uint32_t>(
            chat["max_message_length"].value_or(int64_t{2000}));
        chat_config.history_page_size  = static_cast<uint32_t>(
            chat["history_page_size"].value_or(int64_t{50}));
    }

    // KafkaConfig
    apex::shared::adapters::kafka::KafkaConfig kafka_cfg;
    if (auto kafka = tbl["kafka"]; kafka) {
        kafka_cfg.brokers        = kafka["brokers"]
            .value_or(std::string{"localhost:9092"});
        kafka_cfg.consumer_group = kafka["consumer_group"]
            .value_or(std::string{"chat-svc"});
    }
    // Ensure request topic is in consume_topics
    kafka_cfg.consume_topics = {chat_config.request_topic};

    // RedisConfig (data) -- Redis #2 for room membership/online status
    apex::shared::adapters::redis::RedisConfig redis_data_cfg;
    if (auto redis_data = tbl["redis_data"]; redis_data) {
        redis_data_cfg.host     = redis_data["host"]
            .value_or(std::string{"localhost"});
        redis_data_cfg.port     = static_cast<uint16_t>(
            redis_data["port"].value_or(int64_t{6379}));
        redis_data_cfg.db       = static_cast<uint32_t>(
            redis_data["db"].value_or(int64_t{0}));
        redis_data_cfg.password = redis_data["password"]
            .value_or(std::string{});
    }

    // RedisConfig (pubsub) -- Redis #3 for pub/sub broadcast
    apex::shared::adapters::redis::RedisConfig redis_pubsub_cfg;
    if (auto redis_pubsub = tbl["redis_pubsub"]; redis_pubsub) {
        redis_pubsub_cfg.host     = redis_pubsub["host"]
            .value_or(std::string{"localhost"});
        redis_pubsub_cfg.port     = static_cast<uint16_t>(
            redis_pubsub["port"].value_or(int64_t{6379}));
        redis_pubsub_cfg.db       = static_cast<uint32_t>(
            redis_pubsub["db"].value_or(int64_t{0}));
        redis_pubsub_cfg.password = redis_pubsub["password"]
            .value_or(std::string{});
    }

    // PgAdapterConfig
    apex::shared::adapters::pg::PgAdapterConfig pg_cfg;
    if (auto pg = tbl["pg"]; pg) {
        pg_cfg.connection_string  = pg["connection_string"]
            .value_or(std::string{"host=localhost port=6432 dbname=apex user=apex"});
        pg_cfg.pool_size_per_core = static_cast<size_t>(
            pg["pool_size_per_core"].value_or(int64_t{2}));
    }

    // ----------------------------------------------------------------
    // 2. io_context creation (for Kafka co_spawn bridge)
    // ----------------------------------------------------------------
    boost::asio::io_context io_ctx;

    // ----------------------------------------------------------------
    // 3. Adapter creation
    // ----------------------------------------------------------------
    apex::shared::adapters::kafka::KafkaAdapter kafka(kafka_cfg);
    apex::shared::adapters::redis::RedisAdapter redis_data(redis_data_cfg);
    apex::shared::adapters::redis::RedisAdapter redis_pubsub(redis_pubsub_cfg);
    apex::shared::adapters::pg::PgAdapter pg(pg_cfg);

    // ----------------------------------------------------------------
    // 4. ChatService creation -- executor only, no io_context
    // ----------------------------------------------------------------
    apex::chat_svc::ChatService service(
        std::move(chat_config),
        io_ctx.get_executor(),
        kafka,
        redis_data,
        redis_pubsub,
        pg);

    // ----------------------------------------------------------------
    // 5. service.start() -- register handlers to MessageDispatcher
    // ----------------------------------------------------------------
    service.start();

    // ----------------------------------------------------------------
    // 6. Signal handling for graceful shutdown
    // ----------------------------------------------------------------
    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& /*ec*/, int sig) {
        spdlog::info("[ChatService] Received signal {} -- shutting down", sig);
        g_stop_requested.store(true, std::memory_order_release);
        service.stop();
        io_ctx.stop();
    });

    // ----------------------------------------------------------------
    // 7. io_context run on separate thread (coroutine execution)
    // ----------------------------------------------------------------
    std::jthread io_thread([&io_ctx] {
        spdlog::info("[ChatService] io_context thread started");
        io_ctx.run();
        spdlog::info("[ChatService] io_context thread finished");
    });

    // ----------------------------------------------------------------
    // 8. Kafka consumer loop (main thread, blocking)
    //    KafkaConsumer polls via io_context async timer,
    //    so we keep main thread alive until shutdown is requested.
    // ----------------------------------------------------------------
    spdlog::info("[ChatService] Running. Press Ctrl+C to stop.");

    while (!g_stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ----------------------------------------------------------------
    // 9. Graceful shutdown
    // ----------------------------------------------------------------
    spdlog::info("[ChatService] Shutting down...");

    // Ensure io_context is stopped so io_thread can join
    if (!io_ctx.stopped()) {
        io_ctx.stop();
    }

    // jthread auto-joins on destruction

    spdlog::info("Chat Service stopped.");
    return EXIT_SUCCESS;
}
