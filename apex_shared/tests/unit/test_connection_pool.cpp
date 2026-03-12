#include <apex/shared/adapters/connection_pool.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;

struct FakeConnection {
    int id = 0;
    bool valid = true;
};

class FakePool : public ConnectionPool<FakePool, FakeConnection> {
public:
    FakePool(PoolConfig config = {.min_size = 1, .max_size = 4})
        : ConnectionPool(config) {}

    FakeConnection do_create_connection() {
        return {next_id_++, true};
    }

    void do_destroy_connection(FakeConnection& /*conn*/) {
        ++destroy_count;
    }

    bool do_validate(FakeConnection& conn) {
        return conn.valid;
    }

    int next_id_ = 1;
    int destroy_count = 0;
};

TEST(ConnectionPool, AcquireCreatesConnection) {
    FakePool pool;
    auto result = pool.acquire();
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) return;
    EXPECT_EQ(result->id, 1);
    EXPECT_EQ(pool.active_count(), static_cast<size_t>(1));
    EXPECT_EQ(pool.total_count(), static_cast<size_t>(1));
}

TEST(ConnectionPool, ReleaseReturnsToIdle) {
    FakePool pool;
    auto result = pool.acquire();
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) return;
    FakeConnection conn = std::move(*result);
    pool.release(std::move(conn));
    EXPECT_EQ(pool.active_count(), static_cast<size_t>(0));
    EXPECT_EQ(pool.idle_count(), static_cast<size_t>(1));
}

TEST(ConnectionPool, AcquireReusesIdleConnection) {
    FakePool pool;
    auto r1 = pool.acquire();
    EXPECT_TRUE(r1.has_value());
    if (!r1.has_value()) return;
    int id = r1->id;
    FakeConnection conn = std::move(*r1);
    pool.release(std::move(conn));

    auto r2 = pool.acquire();
    EXPECT_TRUE(r2.has_value());
    if (!r2.has_value()) return;
    EXPECT_EQ(r2->id, id);  // 같은 커넥션 재사용
    EXPECT_EQ(pool.total_count(), static_cast<size_t>(1));
}

TEST(ConnectionPool, AcquireFailsWhenExhausted) {
    FakePool pool({.min_size = 1, .max_size = 1});
    auto r1 = pool.acquire();
    EXPECT_TRUE(r1.has_value());
    auto r2 = pool.acquire();
    EXPECT_FALSE(r2.has_value());
}

TEST(ConnectionPool, InvalidConnectionIsDestroyed) {
    FakePool pool;
    auto r1 = pool.acquire();
    EXPECT_TRUE(r1.has_value());
    if (!r1.has_value()) return;
    FakeConnection conn = std::move(*r1);
    conn.valid = false;
    pool.release(std::move(conn));

    auto r2 = pool.acquire();
    EXPECT_TRUE(r2.has_value());
    if (!r2.has_value()) return;
    EXPECT_EQ(r2->id, 2);  // 새 커넥션 생성됨
    EXPECT_EQ(pool.destroy_count, 1);
}

TEST(ConnectionPool, HealthCheckRemovesInvalid) {
    FakePool pool;
    auto r1 = pool.acquire();
    EXPECT_TRUE(r1.has_value());
    if (!r1.has_value()) return;
    FakeConnection conn = std::move(*r1);
    conn.valid = false;
    pool.release(std::move(conn));

    pool.health_check_tick();
    EXPECT_EQ(pool.idle_count(), static_cast<size_t>(0));
    EXPECT_EQ(pool.destroy_count, 1);
}

TEST(ConnectionPool, CloseAllCleansUp) {
    FakePool pool;
    auto r1 = pool.acquire();
    auto r2 = pool.acquire();
    EXPECT_TRUE(r1.has_value());
    EXPECT_TRUE(r2.has_value());
    if (!r1.has_value() || !r2.has_value()) return;
    FakeConnection c1 = std::move(*r1);
    FakeConnection c2 = std::move(*r2);
    pool.release(std::move(c1));
    pool.release(std::move(c2));
    pool.close_all();
    EXPECT_EQ(pool.idle_count(), static_cast<size_t>(0));
    EXPECT_EQ(pool.total_count(), static_cast<size_t>(0));
}

// --- PoolStats tests ---

TEST(ConnectionPool, PoolStatsAcquireRelease) {
    FakePool pool({.min_size = 1, .max_size = 4});
    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    pool.release(std::move(*c1));

    const auto& stats = pool.stats();
    EXPECT_EQ(stats.total_acquired, 1u);
    EXPECT_EQ(stats.total_released, 1u);
    EXPECT_EQ(stats.total_created, 1u);
    EXPECT_EQ(stats.total_destroyed, 0u);
    EXPECT_EQ(stats.total_failed, 0u);
}

TEST(ConnectionPool, PoolStatsExhaustedIncrementsFailCount) {
    FakePool pool({.min_size = 0, .max_size = 1});
    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    auto c2 = pool.acquire();
    EXPECT_FALSE(c2.has_value());
    EXPECT_EQ(pool.stats().total_failed, 1u);
    pool.release(std::move(*c1));
}

TEST(ConnectionPool, PoolStatsDestroyOnInvalidValidation) {
    FakePool pool({.min_size = 0, .max_size = 4});
    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    c1->valid = false;
    pool.release(std::move(*c1));

    // acquire pulls from idle, validates, destroys invalid, creates new
    auto c2 = pool.acquire();
    ASSERT_TRUE(c2.has_value());

    const auto& stats = pool.stats();
    EXPECT_EQ(stats.total_created, 2u);
    EXPECT_EQ(stats.total_destroyed, 1u);
}

TEST(ConnectionPool, PoolStatsMultipleAcquireReleaseCycles) {
    FakePool pool({.min_size = 0, .max_size = 4});

    // 3 acquire-release cycles
    for (int i = 0; i < 3; ++i) {
        auto c = pool.acquire();
        ASSERT_TRUE(c.has_value());
        pool.release(std::move(*c));
    }

    const auto& stats = pool.stats();
    EXPECT_EQ(stats.total_acquired, 3u);
    EXPECT_EQ(stats.total_released, 3u);
    EXPECT_EQ(stats.total_created, 1u);  // reused from idle
    EXPECT_EQ(stats.total_destroyed, 0u);
    EXPECT_EQ(stats.total_failed, 0u);
}

// --- discard() tests ---

TEST(ConnectionPool, DiscardDecrementsCounters) {
    FakePool pool({.min_size = 0, .max_size = 4});
    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    EXPECT_EQ(pool.active_count(), static_cast<size_t>(1));
    EXPECT_EQ(pool.total_count(), static_cast<size_t>(1));

    pool.discard(std::move(*c1));
    EXPECT_EQ(pool.active_count(), static_cast<size_t>(0));
    EXPECT_EQ(pool.total_count(), static_cast<size_t>(0));
    EXPECT_EQ(pool.stats().total_destroyed, 1u);
}

TEST(ConnectionPool, DiscardAllowsNewAcquire) {
    FakePool pool({.min_size = 0, .max_size = 1});
    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());

    // Pool exhausted
    auto c2 = pool.acquire();
    EXPECT_FALSE(c2.has_value());

    // Discard frees the slot
    pool.discard(std::move(*c1));

    // Now acquire succeeds again
    auto c3 = pool.acquire();
    EXPECT_TRUE(c3.has_value());
    EXPECT_EQ(pool.stats().total_destroyed, 1u);
    EXPECT_EQ(pool.stats().total_created, 2u);
}
