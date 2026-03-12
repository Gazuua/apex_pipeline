#include <apex/shared/adapters/redis/redis_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/core/core_engine.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::redis;
using namespace apex::shared::adapters;
using apex::core::CoreEngine;
using apex::core::CoreEngineConfig;

TEST(RedisAdapter, Name) {
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_EQ(adapter.name(), "redis");
}

TEST(RedisAdapter, NotReadyBeforeInit) {
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_FALSE(adapter.is_ready());
}

TEST(RedisAdapter, InitCreatesPerCorePools) {
    RedisConfig config{
        .host = "localhost",
        .port = 6379,
        .pool_size_per_core = 2,
    };
    RedisAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 4, .mpsc_queue_capacity = 64};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    // 4개 코어에 대한 풀이 생성되었는지 확인
    // pool() 접근으로 검증
    EXPECT_NO_THROW((void)adapter.pool(0));
    EXPECT_NO_THROW((void)adapter.pool(1));
    EXPECT_NO_THROW((void)adapter.pool(2));
    EXPECT_NO_THROW((void)adapter.pool(3));
}

TEST(RedisAdapter, DrainSetsNotReady) {
    RedisConfig config;
    RedisAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
}

TEST(RedisAdapter, CloseCallsCleanup) {
    RedisConfig config;
    RedisAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 1, .mpsc_queue_capacity = 64};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    adapter.drain();
    adapter.close();

    EXPECT_FALSE(adapter.is_ready());
}

TEST(RedisAdapter, TypeErasureViaWrapper) {
    RedisConfig config;
    auto wrapper = std::make_unique<AdapterWrapper<RedisAdapter>>(config);
    apex::core::AdapterInterface* iface = wrapper.get();

    EXPECT_EQ(iface->name(), "redis");
    EXPECT_FALSE(iface->is_ready());

    // get() 접근 가능 (타입 복원)
    RedisAdapter& adapter = wrapper->get();
    EXPECT_EQ(adapter.name(), "redis");
}

TEST(RedisAdapter, ConfigAccessible) {
    RedisConfig config{
        .host = "redis.local",
        .port = 6380,
        .pool_size_per_core = 5,
    };
    RedisAdapter adapter(config);
    EXPECT_EQ(adapter.config().host, "redis.local");
    EXPECT_EQ(adapter.config().port, 6380);
    EXPECT_EQ(adapter.config().pool_size_per_core, 5u);
}

TEST(RedisAdapter, CloseWithoutInit) {
    // init() 없이 close() 호출 시 크래시 없음
    RedisConfig config;
    RedisAdapter adapter(config);
    EXPECT_NO_THROW(adapter.close());
}

TEST(RedisAdapter, DoubleInit) {
    // init() 2번 호출 — 풀이 추가됨 (설계상 1회만 호출해야 함, 하지만 크래시 없어야 함)
    RedisConfig config;
    RedisAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64};
    CoreEngine engine(engine_config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    // 두 번째 init — 크래시 없어야 함
    EXPECT_NO_THROW(adapter.init(engine));
}

TEST(RedisAdapter, ActiveAndIdleConnectionsInitiallyZero) {
    RedisConfig config;
    RedisAdapter adapter(config);

    CoreEngineConfig engine_config{.num_cores = 2, .mpsc_queue_capacity = 64};
    CoreEngine engine(engine_config);

    adapter.init(engine);

    // 초기 상태: 커넥션 없음
    EXPECT_EQ(adapter.active_connections(), 0u);
    EXPECT_EQ(adapter.idle_connections(), 0u);
}
