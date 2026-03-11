#include <apex/core/server.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

class CrossCoreCallTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<Server>(ServerConfig{
            .port = 0,
            .num_cores = 2,
            .heartbeat_timeout_ticks = 0,
            .handle_signals = false,
        });
        server_thread_ = std::thread([this] { server_->run(); });

        // Wait for server to be running (condition-based instead of sleep)
        ASSERT_TRUE(apex::test::wait_for([&] { return server_->running(); }, std::chrono::milliseconds(5000)));
    }

    void TearDown() override {
        server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    std::unique_ptr<Server> server_;
    std::thread server_thread_;
};

TEST_F(CrossCoreCallTest, BasicCallReturnsResult) {
    std::promise<int> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(1, [] {
                return 42;
            });
            EXPECT_TRUE(result.has_value());
            promise.set_value(result.value());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
}

TEST_F(CrossCoreCallTest, VoidCall) {
    std::promise<void> promise;
    auto future = promise.get_future();
    std::atomic<int> value{0};

    boost::asio::co_spawn(server_->core_io_context(0),
        [this, &promise, &value]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(1, [&value] {
                value.store(77, std::memory_order_relaxed);
            });
            EXPECT_TRUE(result.has_value());
            promise.set_value();
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(value.load(), 77);
}

TEST_F(CrossCoreCallTest, PostFireAndForget) {
    std::atomic<int> value{0};

    auto posted = server_->cross_core_post(1, [&value] {
        value.store(99, std::memory_order_relaxed);
    });
    EXPECT_TRUE(posted.has_value());

    ASSERT_TRUE(apex::test::wait_for([&] { return value.load() == 99; }));
}

TEST_F(CrossCoreCallTest, TimeoutReturnsError) {
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(1,
                [] {
                    // NOTE: sleep_for는 코어 스레드를 블로킹하여 io_context-per-core 원칙을 위반하지만,
                    // 타임아웃 테스트 목적으로 허용. 향후 boost::asio::steady_timer로 전환 검토.
                    std::this_thread::sleep_for(50ms);
                    return 0;
                },
                10ms);  // 10ms timeout
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreTimeout);

    // Wait for the timed-out task to finish executing on the target core.
    // If memory safety is broken, this sleep will trigger a use-after-free crash.
    std::this_thread::sleep_for(std::chrono::milliseconds(100) * apex::test::timeout_multiplier());
    // If we reach here, no crash occurred — timed-out task executed safely.
}

TEST_F(CrossCoreCallTest, MultipleSequentialCalls) {
    std::promise<int> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            int sum = 0;
            for (int i = 1; i <= 5; ++i) {
                auto result = co_await server_->cross_core_call(1, [i] {
                    return i * 10;
                });
                EXPECT_TRUE(result.has_value());
                sum += result.value();
            }
            promise.set_value(sum);
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), 150);  // 10+20+30+40+50
}

TEST(CrossCoreCallQueueFullTest, QueueFullReturnsCrossCoreQueueFull) {
    // Use a tiny MPSC queue (capacity=2). With event-driven drain, we need
    // a blocking lambda on core 1 to prevent it from draining the queue.
    auto server = std::make_unique<Server>(ServerConfig{
        .port = 0,
        .num_cores = 2,
        .mpsc_queue_capacity = 2,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
    });
    std::thread server_thread([&] { server->run(); });

    ASSERT_TRUE(apex::test::wait_for([&] { return server->running(); }, std::chrono::milliseconds(5000)));

    // Post a blocking lambda to core 1 — this will be dequeued and block
    // the drain_inbox, preventing further items from being processed.
    std::atomic<bool> blocker_started{false};
    std::atomic<bool> unblock{false};
    auto posted = server->cross_core_post(1, [&blocker_started, &unblock] {
        blocker_started.store(true, std::memory_order_release);
        while (!unblock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    ASSERT_TRUE(posted.has_value());

    // Wait for core 1 to start executing the blocker (confirms it was dequeued)
    ASSERT_TRUE(apex::test::wait_for([&] { return blocker_started.load(std::memory_order_acquire); }));

    // Core 1 is blocked in drain_inbox. Fill the remaining queue (capacity=2).
    auto sentinel = std::make_shared<int>(0);
    for (int i = 0; i < 2; ++i) {
        auto p = server->cross_core_post(1, [sentinel] { /* no-op */ });
        ASSERT_TRUE(p.has_value()) << "post #" << i << " should succeed";
    }

    // Now core 1's queue is full — cross_core_call should fail with QueueFull
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(server->core_io_context(0),
        [&server, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server->cross_core_call(1, [] {
                return 42;
            });
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreQueueFull);

    // Unblock core 1 so shutdown can proceed
    unblock.store(true, std::memory_order_release);

    server->stop();
    if (server_thread.joinable()) server_thread.join();

    EXPECT_EQ(sentinel.use_count(), 1)
        << "Queued tasks were not properly cleaned up during shutdown";
}
