#include <apex/core/adapter_interface.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/pg/pg_adapter.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;
using namespace apex::shared::adapters;
using apex::core::CoreEngine;
using apex::core::CoreEngineConfig;

TEST(PgAdapter, NameIsPg)
{
    PgAdapter adapter;
    EXPECT_EQ(adapter.name(), "pg");
}

TEST(PgAdapter, NotReadyBeforeInit)
{
    PgAdapter adapter;
    EXPECT_FALSE(adapter.is_ready());
}

TEST(PgAdapter, ReadyAfterInit)
{
    PgAdapterConfig config{.pool_size_per_core = 1, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());
}

TEST(PgAdapter, InitCreatesPerCorePools)
{
    PgAdapterConfig config{.pool_size_per_core = 2, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 4, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    // Verify pools exist for each core
    EXPECT_NO_THROW((void)adapter.pool(0));
    EXPECT_NO_THROW((void)adapter.pool(1));
    EXPECT_NO_THROW((void)adapter.pool(2));
    EXPECT_NO_THROW((void)adapter.pool(3));
}

TEST(PgAdapter, DrainMakesNotReady)
{
    PgAdapterConfig config{.pool_size_per_core = 1, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 1, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
}

TEST(PgAdapter, CloseReleasesResources)
{
    PgAdapterConfig config{.pool_size_per_core = 1, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    adapter.drain(); // drain sets ready_ = false
    adapter.close(); // close releases resources
    EXPECT_FALSE(adapter.is_ready());
}

TEST(PgAdapter, CloseWithoutInit)
{
    PgAdapter adapter;
    // close() without init() should not crash
    EXPECT_NO_THROW(adapter.close());
}

TEST(PgAdapter, DrainWithoutInit)
{
    PgAdapter adapter;
    // drain() without init() should not crash
    EXPECT_NO_THROW(adapter.drain());
}

TEST(PgAdapter, FullLifecycle)
{
    PgAdapterConfig config{.pool_size_per_core = 1, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    EXPECT_FALSE(adapter.is_ready());

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());

    adapter.close();
    EXPECT_FALSE(adapter.is_ready());
}

TEST(PgAdapter, ConfigAccessible)
{
    PgAdapterConfig config{
        .connection_string = "host=mydb port=6432",
        .pool_size_per_core = 4,
        .max_idle_time = {},
        .health_check_interval = {},
        .max_acquire_retries = {},
        .retry_backoff = {},
    };
    PgAdapter adapter(config);
    EXPECT_EQ(adapter.config().connection_string, "host=mydb port=6432");
    EXPECT_EQ(adapter.config().pool_size_per_core, 4u);
}

TEST(PgAdapter, TypeErasureViaAdapterWrapper)
{
    auto wrapper = std::make_unique<AdapterWrapper<PgAdapter>>(PgAdapterConfig{});
    apex::core::AdapterInterface* iface = wrapper.get();

    EXPECT_EQ(iface->name(), "pg");
    EXPECT_FALSE(iface->is_ready());

    // Type restoration via get()
    PgAdapter& adapter = wrapper->get();
    EXPECT_EQ(adapter.name(), "pg");
}

TEST(PgAdapter, PoolConfigMatchesPgConfig)
{
    PgAdapterConfig config{.pool_size_per_core = 5, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 1, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);

    // Pool config should propagate from PgAdapterConfig
    auto& pool = adapter.pool(0);
    EXPECT_EQ(pool.config().min_size, 5u);
    EXPECT_EQ(pool.config().max_size, 10u);
}

TEST(PgAdapter, ActiveAndIdleConnectionsInitiallyZero)
{
    PgAdapterConfig config{.pool_size_per_core = 2, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);

    EXPECT_EQ(adapter.active_connections(), 0u);
    EXPECT_EQ(adapter.idle_connections(), 0u);
}

TEST(PgAdapter, ActiveAndIdleWithoutInit)
{
    PgAdapter adapter;
    EXPECT_EQ(adapter.active_connections(), 0u);
    EXPECT_EQ(adapter.idle_connections(), 0u);
}

TEST(PgAdapter, DoubleInit)
{
    PgAdapterConfig config{.pool_size_per_core = 1, .max_idle_time = {}, .health_check_interval = {}, .max_acquire_retries = {}, .retry_backoff = {}};
    PgAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64, .tick_interval = {}, .drain_batch_limit = {}};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    // Second init should not crash (pools will be appended)
    EXPECT_NO_THROW(adapter.init(engine));
}
