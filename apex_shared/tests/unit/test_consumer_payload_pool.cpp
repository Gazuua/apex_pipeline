// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/kafka/consumer_payload_pool.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace apex::shared::adapters::kafka;

TEST(ConsumerPayloadPool, AcquireAndRelease)
{
    constexpr size_t INITIAL = 4;
    ConsumerPayloadPool pool(INITIAL, 256, 16);

    const size_t free_before = pool.free_count();
    EXPECT_EQ(free_before, INITIAL);

    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    {
        auto ptr = pool.acquire(data);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(pool.in_use_count(), 1u);
        EXPECT_EQ(pool.free_count(), INITIAL - 1);
    }
    // shared_ptr destroyed -> buffer returned to pool
    EXPECT_EQ(pool.free_count(), free_before);
    EXPECT_EQ(pool.in_use_count(), 0u);
}

TEST(ConsumerPayloadPool, PoolExhaustion_FallbackAlloc)
{
    constexpr size_t INITIAL = 2;
    ConsumerPayloadPool pool(INITIAL, 128, 0 /* unlimited */);

    std::vector<uint8_t> data = {0xAA, 0xBB};

    // Acquire all initial buffers
    std::vector<ConsumerPayloadPool::PayloadPtr> held;
    for (size_t i = 0; i < INITIAL; ++i)
    {
        held.push_back(pool.acquire(data));
    }
    EXPECT_EQ(pool.free_count(), 0u);
    EXPECT_EQ(pool.fallback_alloc_count(), 0u);

    // Next acquire triggers fallback allocation
    auto extra = pool.acquire(data);
    ASSERT_NE(extra, nullptr);
    EXPECT_GT(pool.fallback_alloc_count(), 0u);
    EXPECT_EQ(pool.in_use_count(), INITIAL + 1);
}

TEST(ConsumerPayloadPool, MetricsAccuracy)
{
    constexpr size_t INITIAL = 4;
    ConsumerPayloadPool pool(INITIAL, 128, 8);

    EXPECT_EQ(pool.free_count(), INITIAL);
    EXPECT_EQ(pool.in_use_count(), 0u);
    EXPECT_EQ(pool.total_created(), INITIAL);
    EXPECT_EQ(pool.acquire_count(), 0u);
    EXPECT_EQ(pool.peak_in_use(), 0u);

    std::vector<uint8_t> data = {0x10};

    // Acquire 3 buffers
    std::vector<ConsumerPayloadPool::PayloadPtr> held;
    for (int i = 0; i < 3; ++i)
    {
        held.push_back(pool.acquire(data));
    }
    EXPECT_EQ(pool.acquire_count(), 3u);
    EXPECT_EQ(pool.in_use_count(), 3u);
    EXPECT_EQ(pool.free_count(), INITIAL - 3);
    EXPECT_EQ(pool.peak_in_use(), 3u);

    // Release one
    held.pop_back();
    EXPECT_EQ(pool.in_use_count(), 2u);
    // peak_in_use should remain at 3
    EXPECT_EQ(pool.peak_in_use(), 3u);

    // Acquire 2 more -> peak should update
    held.push_back(pool.acquire(data));
    held.push_back(pool.acquire(data));
    EXPECT_EQ(pool.in_use_count(), 4u);
    EXPECT_EQ(pool.peak_in_use(), 4u);
    EXPECT_EQ(pool.acquire_count(), 5u);

    // free_count + in_use_count == total_created (when no fallback)
    EXPECT_EQ(pool.free_count() + pool.in_use_count(), pool.total_created());
}

TEST(ConsumerPayloadPool, MaxCountRespected)
{
    constexpr size_t INITIAL = 2;
    constexpr size_t MAX = 4;
    ConsumerPayloadPool pool(INITIAL, 64, MAX);

    std::vector<uint8_t> data = {0xFF};

    // Exhaust well beyond max_count — should still succeed via fallback
    std::vector<ConsumerPayloadPool::PayloadPtr> held;
    for (size_t i = 0; i < MAX + 2; ++i)
    {
        auto ptr = pool.acquire(data);
        ASSERT_NE(ptr, nullptr) << "acquire failed at i=" << i;
        held.push_back(std::move(ptr));
    }
    EXPECT_EQ(pool.in_use_count(), MAX + 2);
}

TEST(ConsumerPayloadPool, PayloadDataIntegrity)
{
    ConsumerPayloadPool pool(4, 256, 8);

    std::vector<uint8_t> original = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    auto ptr = pool.acquire(original);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr->data.size(), original.size());
    EXPECT_EQ(ptr->data, original);

    // Verify span() accessor
    auto sp = ptr->span();
    EXPECT_EQ(sp.size(), original.size());
    EXPECT_EQ(std::vector<uint8_t>(sp.begin(), sp.end()), original);
}

TEST(ConsumerPayloadPool, EmptyPayload_AcquireSucceeds)
{
    ConsumerPayloadPool pool(4, 256, 8);

    std::vector<uint8_t> empty;
    auto ptr = pool.acquire(empty);
    ASSERT_NE(ptr, nullptr);
    EXPECT_TRUE(ptr->data.empty());
    EXPECT_EQ(ptr->span().size(), 0u);
    EXPECT_EQ(pool.in_use_count(), 1u);
}

TEST(ConsumerPayloadPool, ReleaseOrderIndependent)
{
    constexpr size_t INITIAL = 4;
    ConsumerPayloadPool pool(INITIAL, 128, 8);

    std::vector<uint8_t> data = {0x01};

    // Acquire 4 buffers
    auto a = pool.acquire(data);
    auto b = pool.acquire(data);
    auto c = pool.acquire(data);
    auto d = pool.acquire(data);
    EXPECT_EQ(pool.in_use_count(), 4u);
    EXPECT_EQ(pool.free_count(), 0u);

    // Release in LIFO order: d, c, b, a
    d.reset();
    EXPECT_EQ(pool.in_use_count(), 3u);
    c.reset();
    EXPECT_EQ(pool.in_use_count(), 2u);
    b.reset();
    EXPECT_EQ(pool.in_use_count(), 1u);
    a.reset();
    EXPECT_EQ(pool.in_use_count(), 0u);
    EXPECT_EQ(pool.free_count(), INITIAL);

    // Acquire again and release in FIFO order: e, f, g, h -> release e, f, g, h
    auto e = pool.acquire(data);
    auto f = pool.acquire(data);
    auto g = pool.acquire(data);
    auto h = pool.acquire(data);
    EXPECT_EQ(pool.in_use_count(), 4u);

    e.reset();
    EXPECT_EQ(pool.in_use_count(), 3u);
    f.reset();
    EXPECT_EQ(pool.in_use_count(), 2u);
    g.reset();
    EXPECT_EQ(pool.in_use_count(), 1u);
    h.reset();
    EXPECT_EQ(pool.in_use_count(), 0u);
    EXPECT_EQ(pool.free_count(), INITIAL);
}
