#include <apex/core/mpsc_queue.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <set>

using namespace apex::core;

// --- Basic functionality ---

TEST(MpscQueue, ConstructWithCapacity) {
    MpscQueue<int> q(16);
    EXPECT_EQ(q.capacity(), 16u);
    EXPECT_EQ(q.size_approx(), 0u);
    EXPECT_TRUE(q.empty());
}

TEST(MpscQueue, CapacityRoundsUpToPowerOfTwo) {
    MpscQueue<int> q(10);
    EXPECT_EQ(q.capacity(), 16u);
}

TEST(MpscQueue, EnqueueDequeue_SingleItem) {
    MpscQueue<int> q(4);
    auto result = q.enqueue(42);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(q.size_approx(), 1u);

    auto item = q.dequeue();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, 42);
    EXPECT_TRUE(q.empty());
}

TEST(MpscQueue, EnqueueDequeue_FIFO) {
    MpscQueue<int> q(8);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.enqueue(i).has_value());
    }
    for (int i = 0; i < 5; ++i) {
        auto item = q.dequeue();
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(*item, i);
    }
}

TEST(MpscQueue, DequeueEmpty_ReturnsNullopt) {
    MpscQueue<int> q(4);
    EXPECT_FALSE(q.dequeue().has_value());
}

TEST(MpscQueue, Backpressure_WhenFull) {
    MpscQueue<int> q(4);
    for (size_t i = 0; i < q.capacity(); ++i) {
        ASSERT_TRUE(q.enqueue(static_cast<int>(i)).has_value());
    }
    auto result = q.enqueue(999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), QueueError::Full);
}

TEST(MpscQueue, WrapAround) {
    MpscQueue<int> q(4);
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.enqueue(round * 10 + i).has_value());
        }
        for (int i = 0; i < 4; ++i) {
            auto item = q.dequeue();
            ASSERT_TRUE(item.has_value());
            EXPECT_EQ(*item, round * 10 + i);
        }
    }
}

// --- Concurrency ---

TEST(MpscQueue, MultiProducerSingleConsumer) {
    constexpr int kNumProducers = 4;
    constexpr int kItemsPerProducer = 10000;
    MpscQueue<int> q(4096);

    std::vector<std::thread> producers;
    for (int p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&q, p]() {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                int value = p * kItemsPerProducer + i;
                while (!q.enqueue(value).has_value()) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::set<int> received;
    int total = kNumProducers * kItemsPerProducer;
    while (static_cast<int>(received.size()) < total) {
        if (auto item = q.dequeue(); item.has_value()) {
            received.insert(*item);
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : producers) t.join();

    EXPECT_EQ(static_cast<int>(received.size()), total);
}
