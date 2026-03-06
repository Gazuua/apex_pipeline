#include <apex/core/core_engine.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

// Helper: wait for a condition with timeout
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

TEST(CoreEngineTest, DefaultConfig) {
    CoreEngineConfig config;
    EXPECT_EQ(config.num_cores, 0u);
    EXPECT_EQ(config.mpsc_queue_capacity, 65536u);
    EXPECT_EQ(config.drain_interval, 100us);
}

TEST(CoreEngineTest, CreateWithExplicitCores) {
    CoreEngine engine({.num_cores = 2});
    EXPECT_EQ(engine.core_count(), 2u);
    EXPECT_FALSE(engine.running());
}

TEST(CoreEngineTest, RunAndStop) {
    CoreEngine engine({.num_cores = 2});

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));
    EXPECT_TRUE(engine.running());

    engine.stop();
    runner.join();

    EXPECT_FALSE(engine.running());
}

TEST(CoreEngineTest, PostToCore) {
    CoreEngine engine({.num_cores = 2, .drain_interval = 50us});

    std::atomic<uint64_t> received_data{0};
    std::atomic<uint32_t> received_core{UINT32_MAX};

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        received_core.store(core_id, std::memory_order_relaxed);
        received_data.store(msg.data, std::memory_order_relaxed);
    });

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.source_core = 0;
    msg.data = 42;
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(wait_for([&]() { return received_data.load() == 42; }));
    EXPECT_EQ(received_core.load(), 1u);
    EXPECT_EQ(received_data.load(), 42u);

    engine.stop();
    runner.join();
}

TEST(CoreEngineTest, Broadcast) {
    constexpr uint32_t num_cores = 4;
    CoreEngine engine({.num_cores = num_cores, .drain_interval = 50us});

    std::atomic<uint32_t> count{0};

    engine.set_message_handler([&](uint32_t, const CoreMessage&) {
        count.fetch_add(1, std::memory_order_relaxed);
    });

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.data = 99;
    engine.broadcast(msg);

    ASSERT_TRUE(wait_for([&]() { return count.load() >= num_cores; }));
    EXPECT_EQ(count.load(), num_cores);

    engine.stop();
    runner.join();
}

TEST(CoreEngineTest, PostToFullQueueReturnsFalse) {
    // Use a very small queue capacity
    CoreEngine engine({.num_cores = 1, .mpsc_queue_capacity = 4});

    // Queue capacity is rounded up to next power of 2, so 4 stays 4
    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.data = 1;

    // Fill the queue (don't run the engine so nothing drains)
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(engine.post_to(0, msg));
    }

    // Queue should be full now
    EXPECT_FALSE(engine.post_to(0, msg));
}

TEST(CoreEngineTest, IoContextAccessValid) {
    CoreEngine engine({.num_cores = 2});
    // Valid access
    EXPECT_NO_THROW(engine.io_context(0));
    EXPECT_NO_THROW(engine.io_context(1));
    // Out of range
    EXPECT_THROW(engine.io_context(2), std::out_of_range);
}

TEST(CoreEngineTest, PostToInvalidCoreReturnsFalse) {
    CoreEngine engine({.num_cores = 2});
    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.data = 1;
    EXPECT_FALSE(engine.post_to(99, msg));
}

TEST(CoreEngineTest, MultipleInterCoreMessages) {
    CoreEngine engine({.num_cores = 2, .drain_interval = 50us});

    constexpr int num_messages = 100;
    std::atomic<uint64_t> sum{0};
    std::atomic<int> msg_count{0};

    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        sum.fetch_add(msg.data, std::memory_order_relaxed);
        msg_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::thread runner([&]() { engine.run(); });

    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    // Send 100 messages with data = 1..100 to core 1
    uint64_t expected_sum = 0;
    for (int i = 1; i <= num_messages; ++i) {
        CoreMessage msg;
        msg.type = CoreMessage::Type::Custom;
        msg.source_core = 0;
        msg.data = static_cast<uint64_t>(i);
        EXPECT_TRUE(engine.post_to(1, msg));
        expected_sum += i;
    }

    ASSERT_TRUE(wait_for([&]() { return msg_count.load() >= num_messages; }));
    EXPECT_EQ(sum.load(), expected_sum);  // 5050

    engine.stop();
    runner.join();
}
