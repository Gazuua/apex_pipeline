#include <apex/core/core_engine.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::redis;
using namespace apex::shared::adapters;
using apex::core::CoreEngine;
using apex::core::CoreEngineConfig;

TEST(RedisAdapter, Name)
{
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_EQ(adapter.name(), "redis");
}

TEST(RedisAdapter, NotReadyBeforeInit)
{
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_FALSE(adapter.is_ready());
}

TEST(RedisAdapter, InitCreatesPerCoreMultiplexers)
{
    RedisConfig config{
        .host = "localhost",
        .port = 6379,
        .password = {},
        .db = {},
        .connect_timeout = std::chrono::milliseconds{3000},
        .command_timeout = std::chrono::milliseconds{1000},
        .reconnect_max_backoff = std::chrono::milliseconds{30000},
        .max_pending_commands = 4096,
    };

    CoreEngineConfig engine_config{.num_cores = 4,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    // 4 cores should have multiplexers accessible
    EXPECT_NO_THROW((void)adapter.multiplexer(0));
    EXPECT_NO_THROW((void)adapter.multiplexer(1));
    EXPECT_NO_THROW((void)adapter.multiplexer(2));
    EXPECT_NO_THROW((void)adapter.multiplexer(3));
}

TEST(RedisAdapter, DrainSetsNotReady)
{
    RedisConfig config;
    CoreEngineConfig engine_config{.num_cores = 2,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
}

TEST(RedisAdapter, CloseCallsCleanup)
{
    RedisConfig config;
    CoreEngineConfig engine_config{.num_cores = 1,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    adapter.init(engine);
    adapter.drain();
    adapter.close();

    EXPECT_FALSE(adapter.is_ready());
}

TEST(RedisAdapter, TypeErasureViaWrapper)
{
    RedisConfig config;
    auto wrapper = std::make_unique<AdapterWrapper<RedisAdapter>>(config);
    apex::core::AdapterInterface* iface = wrapper.get();

    EXPECT_EQ(iface->name(), "redis");
    EXPECT_FALSE(iface->is_ready());

    // get() access (type restoration)
    RedisAdapter& adapter = wrapper->get();
    EXPECT_EQ(adapter.name(), "redis");
}

TEST(RedisAdapter, ConfigAccessible)
{
    RedisConfig config{
        .host = "redis.local",
        .port = 6380,
        .password = {},
        .db = {},
        .connect_timeout = std::chrono::milliseconds{3000},
        .command_timeout = std::chrono::milliseconds{1000},
        .reconnect_max_backoff = std::chrono::milliseconds{30000},
        .max_pending_commands = 4096,
    };
    RedisAdapter adapter(config);
    EXPECT_EQ(adapter.config().host, "redis.local");
    EXPECT_EQ(adapter.config().port, 6380);
}

TEST(RedisAdapter, CloseWithoutInit)
{
    // close() without init() should not crash
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_NO_THROW(adapter.close());
}

TEST(RedisAdapter, DoubleInit)
{
    // Double init — should not crash
    RedisConfig config;
    CoreEngineConfig engine_config{.num_cores = 2,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    EXPECT_NO_THROW(adapter.init(engine));
}

TEST(RedisAdapter, ActiveConnectionsInitiallyZero)
{
    RedisConfig config;
    CoreEngineConfig engine_config{.num_cores = 2,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    adapter.init(engine);

    // After init(), connect() is called per core.  redisAsyncConnect is
    // non-blocking, so active_connections() may report >0 even without a
    // reachable Redis server (the async handshake hasn't failed yet).
    // Just verify the count doesn't exceed core count.
    EXPECT_LE(adapter.active_connections(), 2u);
}

TEST(RedisAdapter, DrainWithoutInit)
{
    // drain() without init() should not crash
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_NO_THROW(adapter.drain());
}

TEST(RedisAdapter, FullLifecycle)
{
    // init -> drain -> close full lifecycle
    RedisConfig config;
    CoreEngineConfig engine_config{.num_cores = 2,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    EXPECT_FALSE(adapter.is_ready());

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());

    adapter.close();
    EXPECT_FALSE(adapter.is_ready());

    // Re-init after close is not officially supported but should not crash
    EXPECT_NO_THROW(adapter.init(engine));
}

TEST(RedisAdapter, MultiplexerInitialState)
{
    RedisConfig config;
    CoreEngineConfig engine_config{.num_cores = 1,
                                   .mpsc_queue_capacity = 64,
                                   .tick_interval = std::chrono::milliseconds{100},
                                   .drain_batch_limit = 1024};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    adapter.init(engine);

    // Multiplexer exists and connect() has been called during init.
    // connected() may be true (async connect hasn't failed yet) or false
    // (no Redis server available) depending on the environment.
    auto& mux = adapter.multiplexer(0);
    EXPECT_EQ(mux.pending_count(), 0u);
}

TEST(RedisAdapter, ActiveConnectionsWithoutInit)
{
    // active_connections before init should return 0
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_EQ(adapter.active_connections(), 0u);
}
