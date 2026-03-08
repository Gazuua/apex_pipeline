#include <apex/core/core_engine.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
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
    EXPECT_NO_THROW((void)engine.io_context(0));
    EXPECT_NO_THROW((void)engine.io_context(1));
    // Out of range
    EXPECT_THROW((void)engine.io_context(2), std::out_of_range);
}

TEST(CoreEngineTest, StartAndJoin) {
    CoreEngine engine({.num_cores = 2, .drain_interval = 50us});

    std::atomic<uint64_t> received{0};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        received.store(msg.data, std::memory_order_relaxed);
    });

    engine.start();  // non-blocking
    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.data = 777;
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(wait_for([&]() { return received.load() == 777; }));

    engine.stop();
    engine.join();

    EXPECT_FALSE(engine.running());
}

TEST(CoreEngineTest, PostToInvalidCoreReturnsFalse) {
    CoreEngine engine({.num_cores = 2});
    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.data = 1;
    EXPECT_FALSE(engine.post_to(99, msg));
}

TEST(CoreEngineTest, DrainCallback) {
    CoreEngine engine({.num_cores = 2, .drain_interval = 50us});

    std::atomic<uint32_t> drain_count{0};
    engine.set_drain_callback([&](uint32_t) {
        drain_count.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();
    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    // drain_timer fires multiple times (Windows timer resolution ~15.6ms)
    ASSERT_TRUE(wait_for([&]() { return drain_count.load() > 0; }));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, CrossCoreMessageViaHandler) {
    CoreEngine engine({.num_cores = 2, .drain_interval = 50us});

    std::atomic<uint8_t> received_type{255};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        received_type.store(static_cast<uint8_t>(msg.type), std::memory_order_relaxed);
    });

    engine.start();
    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    // Custom messages go through message_handler_
    CoreMessage msg;
    msg.type = CoreMessage::Type::Custom;
    msg.data = 123;
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(wait_for([&]() { return received_type.load() != 255; }));
    EXPECT_EQ(received_type.load(),
              static_cast<uint8_t>(CoreMessage::Type::Custom));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, CrossCoreRequestAutoExecuted) {
    CoreEngine engine({.num_cores = 2, .drain_interval = 50us});

    std::atomic<int> value{0};
    auto* task = new std::function<void()>([&value] {
        value.store(42, std::memory_order_relaxed);
    });

    engine.start();
    ASSERT_TRUE(wait_for([&]() { return engine.running(); }));

    CoreMessage msg;
    msg.type = CoreMessage::Type::CrossCoreRequest;
    msg.data = reinterpret_cast<uint64_t>(task);
    EXPECT_TRUE(engine.post_to(1, msg));

    ASSERT_TRUE(wait_for([&]() { return value.load() == 42; }));

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, DrainRemainingCleansUpPointers) {
    CoreEngine engine({.num_cores = 1, .drain_interval = 50us});

    // Track whether each task's destructor was called by watching an external flag
    auto flag1 = std::make_shared<bool>(false);
    auto flag2 = std::make_shared<bool>(false);

    // CrossCoreRequest: captures shared_ptr, destructor sets flag
    auto* task1 = new std::function<void()>([flag1] { *flag1 = true; });
    CoreMessage msg1;
    msg1.type = CoreMessage::Type::CrossCoreRequest;
    msg1.data = reinterpret_cast<uint64_t>(task1);
    EXPECT_TRUE(engine.post_to(0, msg1));

    // CrossCorePost: same pattern
    auto* task2 = new std::function<void()>([flag2] { *flag2 = true; });
    CoreMessage msg2;
    msg2.type = CoreMessage::Type::CrossCorePost;
    msg2.data = reinterpret_cast<uint64_t>(task2);
    EXPECT_TRUE(engine.post_to(0, msg2));

    // Verify tasks have NOT been executed (engine was never started)
    EXPECT_FALSE(*flag1);
    EXPECT_FALSE(*flag2);

    // drain_remaining deletes std::function<void()>* for CrossCore messages
    engine.drain_remaining();

    // Tasks should NOT have been executed — only deleted
    EXPECT_FALSE(*flag1);
    EXPECT_FALSE(*flag2);

    // After drain, the shared_ptrs captured inside the deleted std::functions
    // should have been released. Since we hold the last reference via flag1/flag2,
    // the shared_ptr ref count should be 1 (only our local copy).
    EXPECT_EQ(flag1.use_count(), 1);
    EXPECT_EQ(flag2.use_count(), 1);
}

TEST(CoreEngineTest, DestructorDrainsRemaining) {
    auto flag = std::make_shared<bool>(false);
    {
        // Engine never started — destructor should still drain queued messages
        CoreEngine engine({.num_cores = 1, .mpsc_queue_capacity = 64});

        auto* task = new std::function<void()>([flag] { *flag = true; });
        CoreMessage msg;
        msg.type = CoreMessage::Type::CrossCorePost;
        msg.data = reinterpret_cast<uint64_t>(task);
        EXPECT_TRUE(engine.post_to(0, msg));
        // ~CoreEngine should call drain_remaining()
    }
    // After destruction, task should be deleted (not executed — drain only deletes)
    EXPECT_FALSE(*flag);
    EXPECT_EQ(flag.use_count(), 1);
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
