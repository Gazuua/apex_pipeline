#include <apex/core/server.hpp>

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
            .drain_interval = std::chrono::microseconds{50},
            .heartbeat_timeout_ticks = 0,
            .handle_signals = false,
        });
        server_thread_ = std::thread([this] { server_->run(); });
        std::this_thread::sleep_for(200ms);
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

    bool posted = server_->cross_core_post(1, [&value] {
        value.store(99, std::memory_order_relaxed);
    });
    EXPECT_TRUE(posted);

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (value.load() != 99 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(value.load(), 99);
}

TEST_F(CrossCoreCallTest, TimeoutReturnsError) {
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(1,
                [] {
                    std::this_thread::sleep_for(500ms);
                    return 0;
                },
                10ms);  // 10ms timeout
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreTimeout);
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
