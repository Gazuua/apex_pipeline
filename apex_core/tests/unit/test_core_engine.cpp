// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_engine.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

TEST(CoreEngineTest, DefaultConfig)
{
    CoreEngineConfig config;
    EXPECT_EQ(config.num_cores, 0u);
    EXPECT_EQ(config.spsc_queue_capacity, 1024u);
    EXPECT_EQ(config.tick_interval, std::chrono::milliseconds(100));
    EXPECT_EQ(config.drain_batch_limit, 1024u);
}

TEST(CoreEngineTest, CreateWithExplicitCores)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});
    EXPECT_EQ(engine.core_count(), 2u);
    EXPECT_FALSE(engine.running());
}

TEST(CoreEngineTest, RunAndStop)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));
    EXPECT_TRUE(engine.running());

    engine.stop();
    runner.join();

    EXPECT_FALSE(engine.running());
}

TEST(CoreEngineTest, PostToCore)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<uint64_t> received_data{0};
    std::atomic<uint32_t> received_core{UINT32_MAX};

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        received_core.store(core_id, std::memory_order_relaxed);
        received_data.store(msg.data, std::memory_order_relaxed);
    });

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.op = CrossCoreOp::Custom;
    msg.source_core = 0;
    msg.data = 42;
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(apex::test::wait_for([&]() { return received_data.load() == 42; }));
    EXPECT_EQ(received_core.load(), 1u);
    EXPECT_EQ(received_data.load(), 42u);

    engine.stop();
    runner.join();
}

TEST(CoreEngineTest, Broadcast)
{
    constexpr uint32_t num_cores = 4;
    CoreEngine engine({.num_cores = num_cores,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<uint32_t> count{0};

    engine.set_message_handler([&](uint32_t, const CoreMessage&) { count.fetch_add(1, std::memory_order_relaxed); });

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.op = CrossCoreOp::Custom;
    msg.data = 99;
    engine.broadcast(msg);

    ASSERT_TRUE(apex::test::wait_for([&]() { return count.load() >= num_cores; }));
    EXPECT_EQ(count.load(), num_cores);

    engine.stop();
    runner.join();
}

TEST(CoreEngineTest, PostToFullSpscQueueReturnsFalse)
{
    // SPSC mesh requires 2+ cores. Use small capacity.
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 4,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    // Start engine so core threads set tls_core_id_
    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    // Post from core 0 to core 1 via co_spawn (need core thread for SPSC)
    std::atomic<bool> full_detected{false};
    boost::asio::co_spawn(
        engine.io_context(0),
        [&]() -> boost::asio::awaitable<void> {
            CoreMessage msg{.op = CrossCoreOp::Custom, .data = 1};
            // Fill SPSC queue (capacity 4)
            for (size_t i = 0; i < 4; ++i)
            {
                auto r = engine.post_to(1, msg);
                if (!r.has_value())
                {
                    full_detected.store(true, std::memory_order_release);
                    co_return;
                }
            }
            // Next post should fail — queue full
            auto r = engine.post_to(1, msg);
            if (!r.has_value())
                full_detected.store(true, std::memory_order_release);
        },
        boost::asio::detached);

    ASSERT_TRUE(apex::test::wait_for([&]() { return full_detected.load(); }));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, IoContextAccessValid)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});
    // Valid access
    EXPECT_NO_THROW((void)engine.io_context(0));
    EXPECT_NO_THROW((void)engine.io_context(1));
    // Out of range
    EXPECT_THROW((void)engine.io_context(2), std::out_of_range);
}

TEST(CoreEngineTest, StartAndJoin)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<uint64_t> received{0};
    engine.set_message_handler(
        [&](uint32_t, const CoreMessage& msg) { received.store(msg.data, std::memory_order_relaxed); });

    engine.start(); // non-blocking
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.op = CrossCoreOp::Custom;
    msg.data = 777;
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(apex::test::wait_for([&]() { return received.load() == 777; }));

    engine.stop();
    engine.join();

    EXPECT_FALSE(engine.running());
}

TEST(CoreEngineTest, PostToInvalidCoreReturnsFalse)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});
    CoreMessage msg;
    msg.op = CrossCoreOp::Custom;
    msg.data = 1;
    EXPECT_FALSE(engine.post_to(99, msg));
}

TEST(CoreEngineTest, TickCallback)
{
    CoreEngine engine({.num_cores = 2, .tick_interval = std::chrono::milliseconds(50), .drain_batch_limit = 1024});

    std::atomic<uint32_t> tick_count{0};
    engine.set_tick_callback([&](uint32_t) { tick_count.fetch_add(1, std::memory_order_relaxed); });

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    // tick_timer fires independently (Windows timer resolution ~15.6ms)
    ASSERT_TRUE(apex::test::wait_for([&]() { return tick_count.load() > 0; }));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, CrossCoreMessageViaHandler)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<uint16_t> received_type{0xFFFF};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        received_type.store(static_cast<uint16_t>(msg.op), std::memory_order_relaxed);
    });

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    // Custom messages go through message_handler_
    CoreMessage msg;
    msg.op = CrossCoreOp::Custom;
    msg.data = 123;
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(apex::test::wait_for([&]() { return received_type.load() != 0xFFFF; }));
    EXPECT_EQ(received_type.load(), static_cast<uint16_t>(CrossCoreOp::Custom));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, CrossCoreRequestAutoExecuted)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<int> value{0};
    auto* task = new std::function<void()>([&value] { value.store(42, std::memory_order_relaxed); });

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.op = CrossCoreOp::LegacyCrossCoreFn;
    msg.data = reinterpret_cast<uintptr_t>(task);
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(apex::test::wait_for([&]() { return value.load() == 42; }));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, DrainRemainingCleansUpPointers)
{
    // Start engine with 2 cores so SPSC mesh is active.
    // Post LegacyCrossCoreFn messages from core 0 to core 1 via SPSC,
    // then stop+join (prevents drain), call drain_remaining() which calls mesh_->shutdown(),
    // and verify the tasks were deleted (not executed) by checking shared_ptr use_count.
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 1024,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    // Shared pointers captured by tasks — use_count tracks whether tasks are deleted
    auto marker1 = std::make_shared<bool>(false);
    auto marker2 = std::make_shared<bool>(false);

    // use_count == 2: one here + one in the lambda/task
    EXPECT_EQ(marker1.use_count(), 1);
    EXPECT_EQ(marker2.use_count(), 1);

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    // From core 0, post LegacyCrossCoreFn tasks to core 1's SPSC queue
    std::atomic<bool> posted{false};
    boost::asio::co_spawn(
        engine.io_context(0),
        [&]() -> boost::asio::awaitable<void> {
            auto* task1 = new std::function<void()>([marker1] { *marker1 = true; });
            auto* task2 = new std::function<void()>([marker2] { *marker2 = true; });

            CoreMessage msg1{
                .op = CrossCoreOp::LegacyCrossCoreFn, .source_core = 0, .data = reinterpret_cast<uintptr_t>(task1)};
            CoreMessage msg2{
                .op = CrossCoreOp::LegacyCrossCoreFn, .source_core = 0, .data = reinterpret_cast<uintptr_t>(task2)};

            (void)engine.post_to(1, msg1);
            (void)engine.post_to(1, msg2);
            posted.store(true, std::memory_order_release);
            co_return;
        },
        boost::asio::detached);

    ASSERT_TRUE(apex::test::wait_for([&]() { return posted.load(std::memory_order_acquire); }));

    // Stop + join immediately — core 1 may not have drained the SPSC messages yet
    engine.stop();
    engine.join();

    // Tasks were captured by lambda → shared_ptr use_count should be 2 if not yet deleted
    // (However, core 1 might have drained some before stop. We check after drain_remaining.)

    // drain_remaining() calls mesh_->shutdown() which deletes remaining LegacyCrossCoreFn tasks
    engine.drain_remaining();

    // After drain_remaining, all tasks should be deleted (either executed during drain or
    // deleted by shutdown). The shared_ptr markers should NOT have been set to true
    // (shutdown deletes without executing), and use_count should be back to 1.
    EXPECT_EQ(marker1.use_count(), 1);
    EXPECT_EQ(marker2.use_count(), 1);
}

TEST(CoreEngineTest, DestructorDrainsRemaining)
{
    // ~CoreEngine → drain_remaining → mesh_->shutdown() cleans SPSC queues.
    // Destructor-level cleanup is integration-tested here.
    // Detailed SpscMesh shutdown cleanup tested in test_spsc_mesh.cpp::Shutdown_CleansLegacyClosures.
    //
    // Verify ~CoreEngine doesn't crash or leak (ASAN catches leaks).
    {
        CoreEngine engine({.num_cores = 2,
                           .spsc_queue_capacity = 64,
                           .tick_interval = std::chrono::milliseconds{100},
                           .drain_batch_limit = 1024});
        // Engine not started — destructor path exercises drain_remaining on empty mesh.
    }
    // If ASAN doesn't report a leak, we're good.
}

TEST(CoreEngineTest, CrossCoreDispatcherPriorityOverMessageHandler)
{
    // When both CrossCoreDispatcher handler and message_handler_ are set,
    // registered ops should go to CrossCoreDispatcher, unregistered ops to message_handler_
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<int> dispatcher_count{0};
    std::atomic<int> handler_count{0};

    engine.register_cross_core_handler(static_cast<CrossCoreOp>(0x0300), [](uint32_t, uint32_t, void* data) {
        static_cast<std::atomic<int>*>(data)->fetch_add(1, std::memory_order_relaxed);
    });

    engine.set_message_handler(
        [&](uint32_t, const CoreMessage&) { handler_count.fetch_add(1, std::memory_order_relaxed); });

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&] { return engine.running(); }));

    // Send registered op → should go to CrossCoreDispatcher
    CoreMessage registered_msg{.op = static_cast<CrossCoreOp>(0x0300),
                               .source_core = 0,
                               .data = reinterpret_cast<uintptr_t>(&dispatcher_count)};
    EXPECT_TRUE(engine.post_to(1, registered_msg));
    ASSERT_TRUE(apex::test::wait_for([&] { return dispatcher_count.load(std::memory_order_acquire) == 1; }));
    EXPECT_EQ(handler_count.load(), 0); // message_handler_ NOT called

    // Send unregistered op → should go to message_handler_
    CoreMessage unregistered_msg{.op = CrossCoreOp::Custom, .source_core = 0, .data = 0};
    EXPECT_TRUE(engine.post_to(1, unregistered_msg));
    ASSERT_TRUE(apex::test::wait_for([&] { return handler_count.load(std::memory_order_acquire) == 1; }));
    EXPECT_EQ(dispatcher_count.load(), 1); // CrossCoreDispatcher NOT called again

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, LegacyCrossCoreFnExceptionDoesNotStopDrain)
{
    // A legacy closure that throws should not prevent subsequent messages from processing
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    std::atomic<int> after_exception{0};

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&] { return engine.running(); }));

    // Post a throwing task first
    auto* bad_task = new std::function<void()>([] { throw std::runtime_error("intentional test exception"); });
    CoreMessage bad_msg;
    bad_msg.op = CrossCoreOp::LegacyCrossCoreFn;
    bad_msg.data = reinterpret_cast<uintptr_t>(bad_task);
    EXPECT_TRUE(engine.post_to(1, bad_msg));

    // Post a normal task after — should still execute despite the exception above
    auto* good_task =
        new std::function<void()>([&after_exception] { after_exception.store(1, std::memory_order_release); });
    CoreMessage good_msg;
    good_msg.op = CrossCoreOp::LegacyCrossCoreFn;
    good_msg.data = reinterpret_cast<uintptr_t>(good_task);
    EXPECT_TRUE(engine.post_to(1, good_msg));

    ASSERT_TRUE(apex::test::wait_for([&] { return after_exception.load(std::memory_order_acquire) == 1; }));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, DoubleStartThrows)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});
    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&] { return engine.running(); }));

    EXPECT_THROW(engine.start(), std::logic_error);

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, MultipleInterCoreMessages)
{
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 65536,
                       .tick_interval = std::chrono::milliseconds{100},
                       .drain_batch_limit = 1024});

    constexpr int num_messages = 100;
    std::atomic<uint64_t> sum{0};
    std::atomic<int> msg_count{0};

    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        sum.fetch_add(msg.data, std::memory_order_relaxed);
        msg_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    // Send 100 messages with data = 1..100 to core 1
    uint64_t expected_sum = 0;
    for (int i = 1; i <= num_messages; ++i)
    {
        CoreMessage msg;
        msg.op = CrossCoreOp::Custom;
        msg.source_core = 0;
        msg.data = static_cast<uintptr_t>(i);
        EXPECT_TRUE(engine.post_to(1, msg));
        expected_sum += i;
    }

    ASSERT_TRUE(apex::test::wait_for([&]() { return msg_count.load() >= num_messages; }));
    EXPECT_EQ(sum.load(), expected_sum); // 5050

    engine.stop();
    runner.join();
}

TEST(CoreEngineTest, CoPostToBackpressure)
{
    // co_post_to should suspend when SPSC queue is full and resume after drain.
    // Use small capacity (2) to trigger backpressure quickly.
    CoreEngine engine({.num_cores = 2,
                       .spsc_queue_capacity = 2,
                       .tick_interval = std::chrono::milliseconds{50},
                       .drain_batch_limit = 1024});

    std::atomic<int> received_count{0};
    engine.set_message_handler(
        [&](uint32_t, const CoreMessage&) { received_count.fetch_add(1, std::memory_order_relaxed); });

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));

    // From core 0: fill SPSC queue to core 1 (2 messages via post_to),
    // then co_await co_post_to (should suspend because queue is full).
    // When core 1 drains messages, producer is notified and co_post_to completes.
    std::atomic<bool> co_post_completed{false};
    boost::asio::co_spawn(
        engine.io_context(0),
        [&]() -> boost::asio::awaitable<void> {
            CoreMessage msg{.op = CrossCoreOp::Custom, .source_core = 0, .data = 1};

            // Fill the SPSC queue (capacity=2)
            (void)engine.post_to(1, msg);
            msg.data = 2;
            (void)engine.post_to(1, msg);

            // Queue is full — co_post_to should suspend until core 1 drains
            msg.data = 3;
            co_await engine.co_post_to(1, msg);

            // If we get here, backpressure resolved
            co_post_completed.store(true, std::memory_order_release);
        },
        boost::asio::detached);

    // Core 1 automatically drains via schedule_drain, which calls notify_producer_if_waiting.
    // The co_post_to should eventually complete.
    ASSERT_TRUE(apex::test::wait_for([&]() { return co_post_completed.load(std::memory_order_acquire); }));

    // All 3 messages should have been received by core 1
    ASSERT_TRUE(apex::test::wait_for([&]() { return received_count.load(std::memory_order_acquire) >= 3; }));
    EXPECT_EQ(received_count.load(), 3);

    engine.stop();
    engine.join();
}
