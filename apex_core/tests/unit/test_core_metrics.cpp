#include <apex/core/core_engine.hpp>
#include <gtest/gtest.h>

using namespace apex::core;

TEST(CoreMetrics, InitialState)
{
    CoreMetrics metrics;
    EXPECT_EQ(metrics.post_total.load(), 0u);
    EXPECT_EQ(metrics.post_failures.load(), 0u);
}

TEST(CoreMetrics, IncrementCounters)
{
    CoreMetrics metrics;
    metrics.post_total.fetch_add(1, std::memory_order_relaxed);
    metrics.post_failures.fetch_add(1, std::memory_order_relaxed);
    EXPECT_EQ(metrics.post_total.load(), 1u);
    EXPECT_EQ(metrics.post_failures.load(), 1u);
}

TEST(CoreMetrics, PostToIncrementsTotal)
{
    CoreEngineConfig config;
    config.num_cores = 2;
    config.mpsc_queue_capacity = 4;
    CoreEngine engine(config);

    CoreMessage msg{};
    auto r = engine.post_to(0, msg);
    ASSERT_TRUE(r.has_value());

    // Verify post_total incremented via public accessor
    EXPECT_EQ(engine.metrics(0).post_total.load(), 1u);
    EXPECT_EQ(engine.metrics(0).post_failures.load(), 0u);
}

TEST(CoreMetrics, PostToFailureIncrementsFailures)
{
    CoreEngineConfig config;
    config.num_cores = 1;
    config.mpsc_queue_capacity = 2; // Very small queue
    CoreEngine engine(config);

    CoreMessage msg{};
    // Fill the queue
    (void)engine.post_to(0, msg);
    (void)engine.post_to(0, msg);

    // Next post should fail — queue full
    auto r = engine.post_to(0, msg);
    EXPECT_FALSE(r.has_value());

    EXPECT_GE(engine.metrics(0).post_total.load(), 3u);
    EXPECT_GE(engine.metrics(0).post_failures.load(), 1u);
}

TEST(CoreMetrics, MetricsAccessorBoundsCheck)
{
    CoreEngineConfig config;
    config.num_cores = 2;
    CoreEngine engine(config);

    // Valid core IDs
    EXPECT_NO_THROW((void)engine.metrics(0));
    EXPECT_NO_THROW((void)engine.metrics(1));

    // Out-of-range core ID
    EXPECT_THROW((void)engine.metrics(2), std::out_of_range);
}
