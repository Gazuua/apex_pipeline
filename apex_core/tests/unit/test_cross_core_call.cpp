#include <apex/core/server.hpp>
#include <apex/core/tcp_binary_protocol.hpp>

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
        });
        server_->listen<TcpBinaryProtocol>(0);
        server_thread_ = std::thread([this] { server_->run(); });

        // Wait for server to be running (condition-based instead of sleep)
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
    std::atomic<int> value{0};

    auto posted = server_->cross_core_post(1, [&value] { value.store(99, std::memory_order_relaxed); });
    EXPECT_TRUE(posted.has_value());

    ASSERT_TRUE(apex::test::wait_for([&] { return value.load() == 99; }));
}

TEST_F(CrossCoreCallTest, TimeoutReturnsError)
{
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(
                1,
                [] {
                    // NOTE: sleep_for는 코어 스레드를 블로킹하여 io_context-per-core 원칙을 위반하지만,
                    // 타임아웃 테스트 목적으로 허용. 향후 boost::asio::steady_timer로 전환 검토.
                    std::this_thread::sleep_for(50ms);
                    return 0;
                },
                10ms); // 10ms timeout
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
    EXPECT_EQ(future.get(), 150); // 10+20+30+40+50
}

TEST(CrossCoreCallQueueFullTest, QueueFullReturnsCrossCoreQueueFull)
{
    // Use a tiny MPSC queue (capacity=2). With event-driven drain, we need
    // a blocking lambda on core 1 to prevent it from draining the queue.
    auto server = std::make_unique<Server>(ServerConfig{
        .num_cores = 2,
        .mpsc_queue_capacity = 2,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
        .drain_timeout = std::chrono::seconds{25},
        .cross_core_call_timeout = std::chrono::milliseconds{5000},
        .bump_capacity_bytes = 64 * 1024,
        .arena_block_bytes = 4096,
        .arena_max_bytes = 1024 * 1024,
    });
    server->listen<TcpBinaryProtocol>(0);
    std::thread server_thread([&] { server->run(); });

    ASSERT_TRUE(apex::test::wait_for([&] { return server->running(); }, std::chrono::milliseconds(5000)));

    // Post a blocking lambda to core 1 — this will be dequeued and block
    // the drain_inbox, preventing further items from being processed.
    std::atomic<bool> blocker_started{false};
    std::atomic<bool> unblock{false};
    auto posted = server->cross_core_post(1, [&blocker_started, &unblock] {
        blocker_started.store(true, std::memory_order_release);
        while (!unblock.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    });
    ASSERT_TRUE(posted.has_value());

    // Wait for core 1 to start executing the blocker (confirms it was dequeued)
    ASSERT_TRUE(apex::test::wait_for([&] { return blocker_started.load(std::memory_order_acquire); }));

    // Core 1 is blocked in drain_inbox. Fill the remaining queue (capacity=2).
    auto sentinel = std::make_shared<int>(0);
    for (int i = 0; i < 2; ++i)
    {
        auto p = server->cross_core_post(1, [sentinel] { /* no-op */ });
        ASSERT_TRUE(p.has_value()) << "post #" << i << " should succeed";
    }

    // Now core 1's queue is full — cross_core_call should fail with QueueFull
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server->core_io_context(0),
        [&server, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server->cross_core_call(1, [] { return 42; });
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreQueueFull);

    // Unblock core 1 so shutdown can proceed
    unblock.store(true, std::memory_order_release);

    server->stop();
    if (server_thread.joinable())
        server_thread.join();

    EXPECT_EQ(sentinel.use_count(), 1) << "Queued tasks were not properly cleaned up during shutdown";
}

TEST(CrossCorePostMsgTest, PostMsgFireAndForget)
{
    // CoreEngine directly — register_cross_core_handler must be called before start()
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .mpsc_queue_capacity = 65536,
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

    auto result = cross_core_post_msg(engine, /*source_core=*/0,
                                      /*target_core=*/1, static_cast<CrossCoreOp>(0x0100), &payload);
    EXPECT_TRUE(result.has_value());

    ASSERT_TRUE(apex::test::wait_for([&] { return received_value.load(std::memory_order_acquire) == 42; }));
    EXPECT_EQ(received_source.load(std::memory_order_acquire), 0u);

    engine.stop();
    engine.join();
}

TEST(CoreEngineTest, TlsCoreId)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .mpsc_queue_capacity = 65536,
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

    (void)cross_core_post_msg(engine, 0, 0, static_cast<CrossCoreOp>(0x0101), &id_on_core0);
    (void)cross_core_post_msg(engine, 0, 1, static_cast<CrossCoreOp>(0x0101), &id_on_core1);

    ASSERT_TRUE(apex::test::wait_for([&] {
        return id_on_core0.load(std::memory_order_acquire) != UINT32_MAX &&
               id_on_core1.load(std::memory_order_acquire) != UINT32_MAX;
    }));
    EXPECT_EQ(id_on_core0.load(), 0u);
    EXPECT_EQ(id_on_core1.load(), 1u);

    engine.stop();
    engine.join();
}

TEST_F(CrossCoreCallTest, FuncExceptionResultsInTimeout)
{
    std::promise<ErrorCode> promise;
    auto future = promise.get_future();

    boost::asio::co_spawn(
        server_->core_io_context(0),
        [this, &promise]() -> boost::asio::awaitable<void> {
            auto result = co_await server_->cross_core_call(
                1, []() -> int { throw std::runtime_error("intentional test exception"); },
                100ms); // short timeout
            EXPECT_FALSE(result.has_value());
            promise.set_value(result.error());
        },
        boost::asio::detached);

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), ErrorCode::CrossCoreTimeout);
}

TEST(CrossCorePostMsgTest, InvalidTargetReturnsError)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .mpsc_queue_capacity = 65536,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});
    // Don't start — post_to with invalid target returns error immediately
    auto result = cross_core_post_msg(engine, 0, 99, CrossCoreOp::Custom);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::Unknown);
}

TEST(CrossCorePostMsgTest, QueueFullReturnsError)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 2,
                                       .mpsc_queue_capacity = 2,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});
    // Don't start — fill queue without draining
    for (int i = 0; i < 2; ++i)
    {
        auto r = cross_core_post_msg(engine, 0, 1, CrossCoreOp::Custom);
        EXPECT_TRUE(r.has_value()) << "post #" << i;
    }
    auto r = cross_core_post_msg(engine, 0, 1, CrossCoreOp::Custom);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::CrossCoreQueueFull);
}

TEST(BroadcastTest, PartialFailureReleasesRefcount)
{
    // Small queue — some cores' posts will fail, broadcast should release for them
    CoreEngine engine(CoreEngineConfig{.num_cores = 3,
                                       .mpsc_queue_capacity = 2,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});

    // Fill core 2's queue (don't start engine — no draining)
    for (int i = 0; i < 2; ++i)
    {
        CoreMessage filler{.op = CrossCoreOp::Custom, .source_core = 0, .data = 0};
        ASSERT_TRUE(engine.post_to(2, filler));
    }

    // Register handler that releases the payload
    engine.register_cross_core_handler(static_cast<CrossCoreOp>(0x0200), [](uint32_t, uint32_t, void* data) {
        auto* p = static_cast<SharedPayload*>(data);
        p->release();
    });

    engine.start();
    ASSERT_TRUE(apex::test::wait_for([&] { return engine.running(); }));

    std::atomic<int> destroyed{0};
    struct TrackPayload : SharedPayload
    {
        std::atomic<int>* flag;
        explicit TrackPayload(std::atomic<int>* f)
            : flag(f)
        {}
        ~TrackPayload() override
        {
            flag->store(1, std::memory_order_release);
        }
    };

    auto* data = new TrackPayload(&destroyed);
    // source=0, targets=1,2. Core 2's queue is full → post fails → broadcast releases for it
    data->set_refcount(2);
    broadcast_cross_core(engine, 0, static_cast<CrossCoreOp>(0x0200), data);

    // Despite core 2 failing, refcount should reach 0 and payload should be deleted
    ASSERT_TRUE(apex::test::wait_for([&] { return destroyed.load(std::memory_order_acquire) == 1; }));

    engine.stop();
    engine.join();
    engine.drain_remaining();
}

TEST(BroadcastTest, BroadcastToAllCores)
{
    CoreEngine engine(CoreEngineConfig{.num_cores = 4,
                                       .mpsc_queue_capacity = 65536,
                                       .tick_interval = std::chrono::milliseconds{100},
                                       .drain_batch_limit = 1024});

    // External counter — survives SharedPayload deletion
    std::atomic<int> received_count{0};

    engine.register_cross_core_handler(static_cast<CrossCoreOp>(0x0102),
                                       [](uint32_t /*core_id*/, uint32_t /*source*/, void* data) {
                                           auto* payload = static_cast<SharedPayload*>(data);
                                           // Counter is stored externally; payload->release() may delete payload
                                           payload->release();
                                       });

    engine.start();

    // SharedPayload subclass that increments external counter on destruction
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
    auto* data = new BroadcastData(&received_count);
    data->set_refcount(3); // 4 cores - 1 source = 3

    broadcast_cross_core(engine, /*source_core=*/0, static_cast<CrossCoreOp>(0x0102), data);

    // Wait for destructor to fire (refcount reaches 0 → delete → sets counter=1)
    ASSERT_TRUE(
        apex::test::wait_for([&received_count] { return received_count.load(std::memory_order_acquire) == 1; }));

    engine.stop();
    engine.join();
}
