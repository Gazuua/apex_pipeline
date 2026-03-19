#include <apex/core/result.hpp>
#include <apex/shared/adapters/pool_concept.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;

// Mock pool type that satisfies PoolLike concept
struct MockConnection
{
    int id = 0;
};

struct MockPool
{
    using Connection = MockConnection;

    apex::core::Result<Connection> acquire()
    {
        return Connection{42};
    }

    void release(Connection /*conn*/) {}
    void discard(Connection /*conn*/) {}
    void close_all() {}

    PoolStats stats() const
    {
        return stats_;
    }

  private:
    PoolStats stats_;
};

// Compile-time concept verification
static_assert(PoolLike<MockPool>, "MockPool must satisfy PoolLike concept");

TEST(PoolConcept, MockPoolSatisfiesConcept)
{
    // Compile-time verification via static_assert is sufficient
    SUCCEED();
}

TEST(PoolConcept, PoolStatsDefaultValues)
{
    PoolStats stats;
    EXPECT_EQ(stats.total_acquired, 0u);
    EXPECT_EQ(stats.total_released, 0u);
    EXPECT_EQ(stats.total_created, 0u);
    EXPECT_EQ(stats.total_destroyed, 0u);
    EXPECT_EQ(stats.total_failed, 0u);
}

TEST(PoolConcept, PoolConfigDefaultValues)
{
    PoolConfig config;
    EXPECT_EQ(config.min_size, 1u);
    EXPECT_EQ(config.max_size, 8u);
    EXPECT_EQ(config.max_idle_time, std::chrono::seconds{60});
    EXPECT_EQ(config.health_check_interval, std::chrono::seconds{30});
}
