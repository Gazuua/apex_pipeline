// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_queue.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <gtest/gtest.h>

#include <array>
#include <thread>

namespace apex::core
{

class SpscQueueTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_ctx_{1};
};

// === 기본 FIFO ===

TEST_F(SpscQueueTest, EnqueueDequeue_FIFO)
{
    SpscQueue<int> q(4, io_ctx_);

    EXPECT_TRUE(q.try_enqueue(10));
    EXPECT_TRUE(q.try_enqueue(20));
    EXPECT_TRUE(q.try_enqueue(30));

    auto v1 = q.try_dequeue();
    auto v2 = q.try_dequeue();
    auto v3 = q.try_dequeue();

    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v3.has_value());

    EXPECT_EQ(*v1, 10);
    EXPECT_EQ(*v2, 20);
    EXPECT_EQ(*v3, 30);
}

TEST_F(SpscQueueTest, DequeueEmpty_ReturnsNullopt)
{
    SpscQueue<int> q(4, io_ctx_);
    EXPECT_FALSE(q.try_dequeue().has_value());
}

TEST_F(SpscQueueTest, EnqueueFull_ReturnsFalse)
{
    SpscQueue<int> q(2, io_ctx_);
    EXPECT_TRUE(q.try_enqueue(1));
    EXPECT_TRUE(q.try_enqueue(2));
    EXPECT_FALSE(q.try_enqueue(3));
}

TEST_F(SpscQueueTest, CapacityRoundsUpToPowerOf2)
{
    SpscQueue<int> q(3, io_ctx_);
    EXPECT_EQ(q.capacity(), 4u);
}

TEST_F(SpscQueueTest, WrapAround)
{
    SpscQueue<int> q(4, io_ctx_);
    for (int round = 0; round < 3; ++round)
    {
        for (int i = 0; i < 4; ++i)
            EXPECT_TRUE(q.try_enqueue(round * 10 + i));
        EXPECT_FALSE(q.try_enqueue(999));
        for (int i = 0; i < 4; ++i)
        {
            auto v = q.try_dequeue();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(*v, round * 10 + i);
        }
        EXPECT_TRUE(q.empty());
    }
}

TEST_F(SpscQueueTest, SizeApprox)
{
    SpscQueue<int> q(8, io_ctx_);
    EXPECT_EQ(q.size_approx(), 0u);
    q.try_enqueue(1);
    q.try_enqueue(2);
    EXPECT_EQ(q.size_approx(), 2u);
    (void)q.try_dequeue();
    EXPECT_EQ(q.size_approx(), 1u);
}

TEST_F(SpscQueueTest, DrainBatch)
{
    SpscQueue<int> q(8, io_ctx_);
    for (int i = 0; i < 5; ++i)
        q.try_enqueue(i);

    std::array<int, 8> batch{};
    size_t count = q.drain(batch);
    EXPECT_EQ(count, 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(batch[i], i);

    EXPECT_TRUE(q.empty());
}

TEST_F(SpscQueueTest, CoreMessage_TrivialCopy)
{
    struct Msg
    {
        uint16_t op;
        uint32_t src;
        uintptr_t data;
    };
    static_assert(std::is_trivially_copyable_v<Msg>);

    SpscQueue<Msg> q(4, io_ctx_);
    EXPECT_TRUE(q.try_enqueue({1, 2, 0x1234}));
    auto msg = q.try_dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->op, 1);
    EXPECT_EQ(msg->src, 2u);
    EXPECT_EQ(msg->data, 0x1234u);
}

// === 동시성 (TSAN) ===

TEST_F(SpscQueueTest, ConcurrentProducerConsumer_TSAN)
{
    constexpr int COUNT = 10000;
    SpscQueue<int> q(1024, io_ctx_);

    std::thread producer([&] {
        for (int i = 0; i < COUNT; ++i)
        {
            while (!q.try_enqueue(i))
            {
                std::this_thread::yield();
            }
        }
    });

    int received = 0;
    int last = -1;
    while (received < COUNT)
    {
        auto v = q.try_dequeue();
        if (v)
        {
            EXPECT_EQ(*v, last + 1) << "FIFO violation at index " << received;
            last = *v;
            ++received;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    producer.join();
}

// === Await Backpressure ===

TEST_F(SpscQueueTest, AwaitEnqueue_SuspendsAndResumes)
{
    SpscQueue<int> q(2, io_ctx_);
    q.try_enqueue(1);
    q.try_enqueue(2);

    bool resumed = false;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            co_await q.enqueue(3);
            resumed = true;
        },
        boost::asio::detached);

    // Start coroutine — should suspend on full queue
    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_FALSE(resumed);

    // Consumer drains one slot
    (void)q.try_dequeue();
    q.notify_producer_if_waiting();

    // Resume the producer
    io_ctx_.restart();
    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(resumed);

    // Verify item 3 was enqueued after item 2
    (void)q.try_dequeue(); // item 2
    auto v = q.try_dequeue();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 3);
}

TEST_F(SpscQueueTest, AwaitEnqueue_ImmediateIfNotFull)
{
    SpscQueue<int> q(4, io_ctx_);

    bool completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            co_await q.enqueue(42);
            completed = true;
        },
        boost::asio::detached);

    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(completed);
    EXPECT_EQ(*q.try_dequeue(), 42);
}

TEST_F(SpscQueueTest, AwaitEnqueue_ReCheckPreventsLostWakeup)
{
    SpscQueue<int> q(2, io_ctx_);
    q.try_enqueue(1);
    q.try_enqueue(2);

    // Consumer drains BEFORE producer suspends
    (void)q.try_dequeue();

    bool completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            co_await q.enqueue(3);
            completed = true;
        },
        boost::asio::detached);

    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(completed); // Re-check should detect space without suspend
}

TEST_F(SpscQueueTest, CancelWaitingProducer)
{
    SpscQueue<int> q(1, io_ctx_);
    q.try_enqueue(1);

    bool caught_abort = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            try
            {
                co_await q.enqueue(2);
            }
            catch (const boost::system::system_error& e)
            {
                if (e.code() == boost::asio::error::operation_aborted)
                    caught_abort = true;
            }
        },
        boost::asio::detached);

    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_FALSE(caught_abort); // suspended

    q.cancel_waiting_producer();
    io_ctx_.restart();
    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(caught_abort);
}

TEST_F(SpscQueueTest, DrainEmpty_ReturnsZero)
{
    SpscQueue<int> q(8, io_ctx_);
    std::array<int, 8> batch{};
    size_t count = q.drain(batch);
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(q.empty());
}

TEST_F(SpscQueueTest, DrainPartial_BatchSmallerThanQueueSize)
{
    SpscQueue<int> q(8, io_ctx_);
    for (int i = 0; i < 6; ++i)
        q.try_enqueue(i * 10);

    // Drain with batch size 3 — should only drain 3 items
    std::array<int, 3> batch{};
    size_t count = q.drain(batch);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(q.size_approx(), 3u);

    // Verify first 3 items drained in FIFO order
    EXPECT_EQ(batch[0], 0);
    EXPECT_EQ(batch[1], 10);
    EXPECT_EQ(batch[2], 20);
}

} // namespace apex::core
