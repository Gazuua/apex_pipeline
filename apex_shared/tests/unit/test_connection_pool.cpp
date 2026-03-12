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
