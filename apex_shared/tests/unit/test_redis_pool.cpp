#include <apex/shared/adapters/redis/redis_pool.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/connection_pool.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::redis;
using namespace apex::shared::adapters;

// --- FakeRedisPool: ConnectionPool CRTP 로직을 mock 커넥션으로 검증 ---

struct FakeRedisConn {
    int id = 0;
    bool valid = true;
    bool connected = true;
};

class FakeRedisPool : public ConnectionPool<FakeRedisPool, FakeRedisConn> {
public:
    explicit FakeRedisPool(PoolConfig config = {.min_size = 1, .max_size = 4})
        : ConnectionPool(config) {}

    FakeRedisConn do_create_connection() {
        return {next_id_++, true, true};
    }
    void do_destroy_connection(FakeRedisConn& /*conn*/) { ++destroy_count; }
    bool do_validate(FakeRedisConn& conn) { return conn.valid && conn.connected; }

    int next_id_ = 1;
    int destroy_count = 0;
};

// --- ConnectionPool CRTP 테스트 (Redis 풀 패턴) ---

TEST(RedisPool, PoolAcquireAndRelease) {
    FakeRedisPool pool({.min_size = 2, .max_size = 4});

    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    EXPECT_EQ(pool.active_count(), 1u);

    auto c2 = pool.acquire();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(pool.active_count(), 2u);

    pool.release(std::move(*c1));
    EXPECT_EQ(pool.active_count(), 1u);
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(RedisPool, PoolExhaustion) {
    FakeRedisPool pool({.min_size = 1, .max_size = 2});

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());

    // 풀 소진
    auto c3 = pool.acquire();
    EXPECT_FALSE(c3.has_value());
}

TEST(RedisPool, InvalidConnectionSkipped) {
    FakeRedisPool pool;
    auto conn = pool.acquire();
    ASSERT_TRUE(conn.has_value());
    conn->valid = false;
    pool.release(std::move(*conn));

    // 재 acquire 시 invalid 커넥션 스킵 -> 새 커넥션 생성
    auto conn2 = pool.acquire();
    ASSERT_TRUE(conn2.has_value());
    EXPECT_EQ(conn2->id, 2);
    EXPECT_EQ(pool.destroy_count, 1);
}

TEST(RedisPool, HealthCheckRemovesDisconnected) {
    FakeRedisPool pool;
    auto conn = pool.acquire();
    ASSERT_TRUE(conn.has_value());
    conn->connected = false;
    pool.release(std::move(*conn));

    pool.health_check_tick();
    EXPECT_EQ(pool.idle_count(), 0u);
    EXPECT_EQ(pool.destroy_count, 1);
}

TEST(RedisPool, ShrinkIdleRespectMinSize) {
    FakeRedisPool pool({.min_size = 1, .max_size = 4,
                        .max_idle_time = std::chrono::seconds{0}});
    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    pool.release(std::move(*c1));
    pool.release(std::move(*c2));
    EXPECT_EQ(pool.idle_count(), 2u);

    // max_idle_time=0이면 즉시 만료 -> min_size까지 축소
    pool.shrink_idle();
    EXPECT_EQ(pool.idle_count(), 1u);  // min_size=1이므로 1개 유지
}

TEST(RedisPool, ConfigValues) {
    RedisConfig config{
        .host = "redis.local",
        .port = 6380,
        .pool_size_per_core = 4,
        .pool_max_size_per_core = 8,
    };
    EXPECT_EQ(config.host, "redis.local");
    EXPECT_EQ(config.pool_size_per_core, 4u);
    EXPECT_EQ(config.pool_max_size_per_core, 8u);
}

TEST(RedisPool, CloseAllCleansUp) {
    FakeRedisPool pool({.min_size = 1, .max_size = 4});
    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    pool.release(std::move(*c1));
    pool.release(std::move(*c2));

    pool.close_all();
    EXPECT_EQ(pool.idle_count(), 0u);
    EXPECT_EQ(pool.total_count(), 0u);
    EXPECT_EQ(pool.destroy_count, 2);
}

TEST(RedisPool, ReleaseAfterDrainDoesNotCrash) {
    FakeRedisPool pool({.min_size = 1, .max_size = 4});
    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());

    // Drain: close all idle connections
    pool.close_all();

    // Release after drain should not crash
    pool.release(std::move(*c1));
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(RedisPool, PoolExhaustionReturnsAdapterError) {
    FakeRedisPool pool({.min_size = 1, .max_size = 1});

    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());

    // 풀 소진 시 AdapterError 반환
    auto c2 = pool.acquire();
    EXPECT_FALSE(c2.has_value());
    EXPECT_EQ(c2.error(), apex::core::ErrorCode::AdapterError);
}

TEST(RedisPool, ConcurrentAcquireWithinSingleCore) {
    // 코어 내 여러 코루틴이 동시에 acquire할 때
    // (실제로는 single-threaded이므로 순차적이지만 로직상 안전성 확인)
    FakeRedisPool pool({.min_size = 1, .max_size = 4});

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    auto c3 = pool.acquire();
    auto c4 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    ASSERT_TRUE(c3.has_value());
    ASSERT_TRUE(c4.has_value());

    EXPECT_EQ(pool.active_count(), 4u);
    EXPECT_EQ(pool.total_count(), 4u);

    // max_size 초과
    auto c5 = pool.acquire();
    EXPECT_FALSE(c5.has_value());

    // 모두 반환
    pool.release(std::move(*c1));
    pool.release(std::move(*c2));
    pool.release(std::move(*c3));
    pool.release(std::move(*c4));
    EXPECT_EQ(pool.active_count(), 0u);
    EXPECT_EQ(pool.idle_count(), 4u);
}

TEST(RedisPool, AcquireReusesIdleBeforeCreatingNew) {
    FakeRedisPool pool({.min_size = 1, .max_size = 4});

    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    int first_id = c1->id;

    pool.release(std::move(*c1));

    // 재 acquire 시 idle에서 재사용
    auto c2 = pool.acquire();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(c2->id, first_id);  // 같은 커넥션 재사용
    EXPECT_EQ(pool.total_count(), 1u);
}

// --- RedisPool 실제 클래스 컴파일 검증 ---

TEST(RedisPool, RedisPoolCompileCheck) {
    // RedisPool 구조체가 올바르게 컴파일되는지 확인
    // 실제 Redis 서버 없이는 커넥션 생성 불가 -> 구조만 검증
    boost::asio::io_context io_ctx;
    RedisConfig config;
    PoolConfig pool_config{
        .min_size = config.pool_size_per_core,
        .max_size = config.pool_max_size_per_core,
        .max_idle_time = config.max_idle_time,
        .health_check_interval = config.health_check_interval,
    };

    RedisPool pool(io_ctx, config, pool_config);
    EXPECT_EQ(pool.redis_config().host, "localhost");
    EXPECT_EQ(pool.redis_config().port, 6379);
    EXPECT_EQ(pool.idle_count(), 0u);
    EXPECT_EQ(pool.active_count(), 0u);
}
