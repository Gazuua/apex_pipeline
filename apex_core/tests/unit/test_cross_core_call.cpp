// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/server.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace apex::core;
using apex::shared::protocols::tcp::TcpBinaryProtocol;
using namespace std::chrono_literals;

class CrossCoreCallTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        server_ = std::make_unique<Server>(ServerConfig{
            .num_cores = 2,
            .heartbeat_timeout_ticks = 0,
            .handle_signals = false,
            .drain_timeout = std::chrono::seconds{25},
            .cross_core_call_timeout = std::chrono::milliseconds{5000},
            .bump_capacity_bytes = 64 * 1024,
            .arena_block_bytes = 4096,
            .arena_max_bytes = 1024 * 1024,
            .metrics = {},
        });
        server_->listen<TcpBinaryProtocol>(0);
        server_thread_ = std::thread([this] { server_->run(); });

        ASSERT_TRUE(apex::test::wait_for([&] { return server_->running(); }, std::chrono::milliseconds(5000)));
    }

    void TearDown() override
    {
        server_->stop();
        if (server_thread_.joinable())
            server_thread_.join();
    }

    std::unique_ptr<Server> server_;
    std::thread server_thread_;
};

TEST_F(CrossCoreCallTest, BasicCallReturnsResult)
{
    std::promise<int> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(1, [] { return 42; });
            EXPECT_TRUE(result.has_value());
            promise.set_value(result.value());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
}

TEST_F(CrossCoreCallTest, VoidCall)
{
    std::promise<void> promise;
    auto future = promise.get_future();
    std::atomic<int> value{0};

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise, &value]() -> boost::asio::awaitable<void> {
            auto result =
                co_await server_->cross_core_call(1, [&value] { value.store(77, std::memory_order_relaxed); });
            EXPECT_TRUE(result.has_value());
            promise.set_value();
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(value.load(), 77);
}

TEST_F(CrossCoreCallTest, PostFireAndForget)
{
    // cross_core_post is now awaitable — call from core thread via co_spawn
    std::atomic<int> value{0};

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &value]() -> boost::asio::awaitable<void> {
            co_await server_->cross_core_post(1, [&value] { value.store(99, std::memory_order_relaxed); });
        },
        boost::asio::detached);

    ASSERT_TRUE(apex::test::wait_for([&] { return value.load() == 99; }));
}

TEST_F(CrossCoreCallTest, TimeoutReturnsError)
{
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            // 의도적 io_context 스레드 블로킹: 50ms sleep으로 10ms 타임아웃을 확실히 초과시킨다.
            // TSAN 환경에서도 50ms vs 10ms 차이는 충분히 크므로 스케일링 불필요.
            auto result = co_await server_->cross_core_call(
                1,
                [] {
                    std::this_thread::sleep_for(50ms);
                    return 0;
                },
                10ms);
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreTimeout);

    std::this_thread::sleep_for(std::chrono::milliseconds(100) * apex::test::timeout_multiplier());
}

TEST_F(CrossCoreCallTest, MultipleSequentialCalls)
{
    std::promise<int> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            int sum = 0;
            for (int i = 1; i <= 5; ++i)
            {
                auto result = co_await server_->cross_core_call(1, [i] { return i * 10; });
                EXPECT_TRUE(result.has_value());
                sum += result.value();
            }
            promise.set_value(sum);
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), 150);
}

TEST(CrossCorePostMsgTest, PostMsgFireAndForget)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .spsc_queue_capacity = 1024,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});

    std::atomic<int> received_value{0};
    std::atomic<uint32_t> received_source{999};

    engine.register_cross_core_handler(
        static_cast<CrossCoreOp>(0x0100), [](uint32_t /*core_id*/, uint32_t source_core, void* data) {
            auto* pair = static_cast<std::pair<std::atomic<int>*, std::atomic<uint32_t>*>*>(data);
            pair->first->store(42, std::memory_order_release);
            pair->second->store(source_core, std::memory_order_release);
        });

    engine.start();

    std::pair<std::atomic<int>*, std::atomic<uint32_t>*> payload{&received_value, &received_source};

    // cross_core_post_msg is awaitable — call from core thread
    boost::asio::co_spawn(
        engine.io_context(0),
        [&]() -> boost::asio::awaitable<void> {
            co_await cross_core_post_msg(engine, 0, 1, static_cast<CrossCoreOp>(0x0100), &payload);
        },
        boost::asio::detached);

    ASSERT_TRUE(apex::test::wait_for([&] { return received_value.load(std::memory_order_acquire) == 42; }));
    EXPECT_EQ(received_source.load(std::memory_order_acquire), 0u);

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, TlsCoreId)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .spsc_queue_capacity = 1024,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});

    std::atomic<uint32_t> id_on_core0{UINT32_MAX};
    std::atomic<uint32_t> id_on_core1{UINT32_MAX};

    engine.register_cross_core_handler(static_cast<CrossCoreOp>(0x0101),
                                       [](uint32_t /*core_id*/, uint32_t /*source*/, void* data) {
                                           auto* target = static_cast<std::atomic<uint32_t>*>(data);
                                           target->store(CoreEngine::current_core_id(), std::memory_order_release);
                                       });

    engine.start();

    // post_to (sync) from main thread — falls back to asio::post for non-core thread
    (void)engine.post_to(0, CoreMessage{.op = static_cast<CrossCoreOp>(0x0101),
                                        .source_core = 0,
                                        .data = reinterpret_cast<uintptr_t>(&id_on_core0)});
    (void)engine.post_to(1, CoreMessage{.op = static_cast<CrossCoreOp>(0x0101),
                                        .source_core = 0,
                                        .data = reinterpret_cast<uintptr_t>(&id_on_core1)});

    ASSERT_TRUE(apex::test::wait_for([&] {
        return id_on_core0.load(std::memory_order_acquire) != UINT32_MAX &&
               id_on_core1.load(std::memory_order_acquire) != UINT32_MAX;
    }));
    EXPECT_EQ(id_on_core0.load(), 0u);
    EXPECT_EQ(id_on_core1.load(), 1u);

    engine.stop();
    engine.join();
}

TEST_F(CrossCoreCallTest, FuncExceptionReturnsCrossCoreFuncException)
{
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(
                1, []() -> int { throw std::runtime_error("intentional test exception"); }, 100ms);
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreFuncException);
}

TEST(CrossCorePostMsgTest, InvalidTargetReturnsError)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .spsc_queue_capacity = 1024,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});
    // post_to with invalid target still returns error (sync path)
    auto result = engine.post_to(99, CoreMessage{.op = CrossCoreOp::Custom});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::Unknown);
}

TEST(BroadcastTest, BroadcastToAllCores)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 4,
                                       .spsc_queue_capacity = 1024,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});

    std::atomic<int> received_count{0};

    engine.register_cross_core_handler(static_cast<CrossCoreOp>(0x0102),
                                       [](uint32_t /*core_id*/, uint32_t /*source*/, void* data) {
                                           auto* payload = static_cast<SharedPayload*>(data);
                                           payload->release();
                                       });

    engine.start();

    struct BroadcastData : SharedPayload
    {
        std::atomic<int>* counter;
        explicit BroadcastData(std::atomic<int>* c)
            : counter(c)
        {}
        ~BroadcastData() override
        {
            counter->store(1, std::memory_order_release);
        }
    };

    // broadcast_cross_core is awaitable — call from core thread
    boost::asio::co_spawn(
        engine.io_context(0),
        [&]() -> boost::asio::awaitable<void> {
            auto* data = new BroadcastData(&received_count);
            data->set_refcount(3); // 4 cores - 1 source = 3
            co_await broadcast_cross_core(engine, 0, static_cast<CrossCoreOp>(0x0102), data);
        },
        boost::asio::detached);

    ASSERT_TRUE(
        apex::test::wait_for([&received_count] { return received_count.load(std::memory_order_acquire) == 1; }));

    engine.stop();
    engine.join();
}

TEST_F(CrossCoreCallTest, SameCoreCallReturnsTimeout)
{
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    // 동일 코어(0→0) 호출: post는 성공하지만 동일 코어 io_context에서 대기하므로 데드락 → 타임아웃
    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(0, [] { return 0; }, 100ms);
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreTimeout);
}
