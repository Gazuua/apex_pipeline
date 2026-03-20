// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "../test_helpers.hpp"
#include <apex/core/core_engine.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace apex::core;

TEST(DrainTick, PostTriggersImmediateDrain)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 64,
                       .tick_interval = std::chrono::milliseconds(1000),
                       .drain_batch_limit = 1024});

    std::atomic<int> received{0};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        if (msg.op == CrossCoreOp::Custom)
        {
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    engine.start();

    // post_to should trigger drain via post(), not wait for timer
    CoreMessage msg{.op = CrossCoreOp::Custom, .source_core = 0, .data = 42};
    auto result = engine.post_to(1, msg);
    ASSERT_TRUE(result.has_value());

    // Drain should happen nearly immediately (not 1000ms tick)
    ASSERT_TRUE(apex::test::wait_for([&] { return received.load() >= 1; }, std::chrono::milliseconds(200)));

    engine.stop();
    engine.join();
}

TEST(DrainTick, TickCallbackFiresIndependently)
{
    CoreEngine engine({.num_cores = 1,
                       .spsc_queue_capacity = 64,
                       .tick_interval = std::chrono::milliseconds(50),
                       .drain_batch_limit = 1024});

    std::atomic<int> tick_count{0};
    engine.set_tick_callback([&](uint32_t) { tick_count.fetch_add(1, std::memory_order_relaxed); });

    engine.start();

    // No messages posted — tick should still fire
    ASSERT_TRUE(apex::test::wait_for([&] { return tick_count.load() >= 2; }, std::chrono::milliseconds(500)));

    engine.stop();
    engine.join();
}

TEST(DrainTick, BatchLimitPreventsStarvation)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 4096,
                       .tick_interval = std::chrono::milliseconds(1000),
                       .drain_batch_limit = 10});

    std::atomic<int> received{0};
    engine.set_message_handler([&](uint32_t, const CoreMessage&) { received.fetch_add(1, std::memory_order_relaxed); });

    engine.start();

    // Flood 100 messages — should be processed in batches of 10
    for (int i = 0; i < 100; ++i)
    {
        CoreMessage msg{.op = CrossCoreOp::Custom, .source_core = 0, .data = {}};
        (void)engine.post_to(1, msg);
    }

    ASSERT_TRUE(apex::test::wait_for([&] { return received.load() >= 100; }, std::chrono::milliseconds(1000)));

    engine.stop();
    engine.join();
}
