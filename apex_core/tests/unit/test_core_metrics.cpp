// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_engine.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

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
    config.spsc_queue_capacity = 4;
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
    // SPSC mesh requires 2+ cores for queue-full to occur.
    // Must call from core thread to use SPSC path.
    CoreEngineConfig config;
    config.num_cores = 2;
    config.spsc_queue_capacity = 2; // Very small queue
    CoreEngine engine(config);

    engine.start();

    std::atomic<bool> done{false};
    boost::asio::co_spawn(
        engine.io_context(0),
        [&]() -> boost::asio::awaitable<void> {
            CoreMessage msg{};
            // Fill the SPSC queue (core 0 → core 1)
            (void)engine.post_to(1, msg);
            (void)engine.post_to(1, msg);

            // Next post should fail — queue full
            auto r = engine.post_to(1, msg);
            EXPECT_FALSE(r.has_value());
            done.store(true, std::memory_order_release);
            co_return;
        },
        boost::asio::detached);

    // Wait for coroutine to complete
    ASSERT_TRUE(apex::test::wait_for([&] { return done.load(std::memory_order_acquire); }));

    EXPECT_EQ(engine.metrics(1).post_total.load(), 3u);
    EXPECT_EQ(engine.metrics(1).post_failures.load(), 1u);

    engine.stop();
    engine.join();
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
