// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_allocator.hpp>
#include <apex/core/slab_allocator.hpp>
#include <gtest/gtest.h>
#include <set>
#include <stdexcept>
#include <vector>

using namespace apex::core;

// Concept 검증
static_assert(CoreAllocator<SlabAllocator>);
static_assert(Freeable<SlabAllocator>);
static_assert(!Resettable<SlabAllocator>);

TEST(SlabAllocator, Construction)
{
    SlabAllocator pool(64, 100);
    EXPECT_EQ(pool.total_count(), 100u);
    EXPECT_EQ(pool.free_count(), 100u);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_GE(pool.slot_size(), 64u);
}

TEST(SlabAllocator, AllocateAndDeallocate)
{
    SlabAllocator pool(32, 10);
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.allocated_count(), 1u);
    EXPECT_EQ(pool.free_count(), 9u);

    pool.deallocate(p);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_EQ(pool.free_count(), 10u);
}

TEST(SlabAllocator, AllocateAll)
{
    SlabAllocator pool(16, 5);
    std::vector<void*> ptrs;
    for (int i = 0; i < 5; ++i)
    {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.free_count(), 0u);

    // Pool exhausted
    EXPECT_EQ(pool.allocate(), nullptr);

    // Deallocate all
    for (void* p : ptrs)
        pool.deallocate(p);
    EXPECT_EQ(pool.free_count(), 5u);
}

TEST(SlabAllocator, NoDuplicatePointers)
{
    SlabAllocator pool(64, 100);
    std::set<void*> ptrs;
    for (int i = 0; i < 100; ++i)
    {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(ptrs.insert(p).second) << "Duplicate pointer returned";
    }
}

TEST(SlabAllocator, ReuseAfterDeallocate)
{
    SlabAllocator pool(32, 2);
    void* p1 = pool.allocate();
    pool.deallocate(p1);
    void* p2 = pool.allocate();
    EXPECT_EQ(p1, p2); // LIFO free-list reuses same slot
}

// --- TypedSlabAllocator ---

struct TestObj
{
    int x;
    float y;
    TestObj(int x, float y)
        : x(x)
        , y(y)
    {}
};

TEST(SlabAllocator, ZeroInitialCountThrows)
{
    EXPECT_THROW(SlabAllocator(64, 0), std::invalid_argument);
}

TEST(TypedSlabAllocator, ConstructAndDestroy)
{
    TypedSlabAllocator<TestObj> pool(10);
    TestObj* obj = pool.construct(42, 3.14f);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->x, 42);
    EXPECT_FLOAT_EQ(obj->y, 3.14f);
    EXPECT_EQ(pool.allocated_count(), 1u);

    pool.destroy(obj);
    EXPECT_EQ(pool.allocated_count(), 0u);
}

// Double-free 감지 테스트
// Debug/Release 모두 double_free_count_ 증가 + silent return (corruption 방지)
TEST(SlabAllocator, DoubleFreeProtection)
{
    SlabAllocator pool(64, 4);

    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.allocated_count(), 2u);

    // 정상 deallocate
    pool.deallocate(p1);
    EXPECT_EQ(pool.allocated_count(), 1u);
    EXPECT_EQ(pool.free_count(), 3u);
    EXPECT_EQ(pool.double_free_count(), 0u);

    // Double-free — double_free_count 증가, 카운트 변경 없어야 함
    pool.deallocate(p1);
    EXPECT_EQ(pool.allocated_count(), 1u); // 변경 없음
    EXPECT_EQ(pool.free_count(), 3u);      // 변경 없음
    EXPECT_EQ(pool.double_free_count(), 1u);

    // 나머지 정상 deallocate는 여전히 동작해야 함
    pool.deallocate(p2);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_EQ(pool.free_count(), 4u);

    // 풀이 여전히 정상 동작하는지 확인
    void* p3 = pool.allocate();
    EXPECT_NE(p3, nullptr);
    EXPECT_EQ(pool.allocated_count(), 1u);
}

// --- auto-grow + metrics ---

TEST(SlabAllocator, AutoGrowOnExhaustion)
{
    // auto_grow=true, initial=4, grow_chunk=4, max_total=16
    SlabAllocator pool(64, 4, SlabAllocatorConfig{.auto_grow = true, .grow_chunk_size = 4, .max_total_count = 16});
    EXPECT_EQ(pool.total_count(), 4u);

    // 4개 할당 후 exhaustion
    std::vector<void*> ptrs;
    for (int i = 0; i < 4; ++i)
    {
        ptrs.push_back(pool.allocate());
        ASSERT_NE(ptrs.back(), nullptr);
    }
    EXPECT_EQ(pool.free_count(), 0u);
    EXPECT_EQ(pool.grow_count(), 0u);

    // 5번째 할당 → auto-grow 트리거
    void* p5 = pool.allocate();
    ASSERT_NE(p5, nullptr);
    ptrs.push_back(p5);
    EXPECT_EQ(pool.total_count(), 8u); // 4 + 4(grow_chunk)
    EXPECT_EQ(pool.grow_count(), 1u);

    // 나머지 3개 할당 (total=8 사용)
    for (int i = 0; i < 3; ++i)
    {
        ptrs.push_back(pool.allocate());
        ASSERT_NE(ptrs.back(), nullptr);
    }
    // total=8, allocated=8 → 다시 grow → total=12
    ptrs.push_back(pool.allocate());
    ASSERT_NE(ptrs.back(), nullptr);
    EXPECT_EQ(pool.total_count(), 12u);

    // total=12, fill up → grow → total=16
    for (int i = 0; i < 3; ++i)
    {
        ptrs.push_back(pool.allocate());
        ASSERT_NE(ptrs.back(), nullptr);
    }
    ptrs.push_back(pool.allocate());
    ASSERT_NE(ptrs.back(), nullptr);
    EXPECT_EQ(pool.total_count(), 16u);

    // fill remaining
    for (int i = 0; i < 3; ++i)
    {
        ptrs.push_back(pool.allocate());
        ASSERT_NE(ptrs.back(), nullptr);
    }

    // total=16, allocated=16 → max → nullptr
    EXPECT_EQ(pool.allocate(), nullptr);
    EXPECT_EQ(pool.total_count(), 16u);

    for (void* p : ptrs)
        pool.deallocate(p);
}

TEST(SlabAllocator, MetricsTracking)
{
    SlabAllocator pool(64, 10, SlabAllocatorConfig{.auto_grow = false, .grow_chunk_size = {}, .max_total_count = {}});

    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    void* p3 = pool.allocate();
    EXPECT_EQ(pool.peak_allocated(), 3u);

    pool.deallocate(p2);
    EXPECT_EQ(pool.peak_allocated(), 3u); // high water mark 유지

    pool.deallocate(p1);
    pool.deallocate(p3);
    EXPECT_EQ(pool.peak_allocated(), 3u); // 여전히 3
}

TEST(SlabAllocator, AutoGrowDisabledByDefault)
{
    SlabAllocator pool(64, 4); // 기존 생성자 — auto_grow=false
    for (int i = 0; i < 4; ++i)
        (void)pool.allocate();
    EXPECT_EQ(pool.allocate(), nullptr); // 기존 동작 유지
}

TEST(SlabAllocator, AutoGrowDefaultChunkSize)
{
    // grow_chunk_size=0 → initial_count 크기로 grow
    SlabAllocator pool(64, 8, SlabAllocatorConfig{.auto_grow = true, .grow_chunk_size = 0, .max_total_count = {}});
    std::vector<void*> ptrs;
    for (int i = 0; i < 8; ++i)
        ptrs.push_back(pool.allocate());
    EXPECT_EQ(pool.total_count(), 8u);

    // 9번째 → grow(8)
    ptrs.push_back(pool.allocate());
    ASSERT_NE(ptrs.back(), nullptr);
    EXPECT_EQ(pool.total_count(), 16u);

    for (void* p : ptrs)
        pool.deallocate(p);
}

// owns()가 슬롯 정렬을 검증하는지 확인
TEST(SlabAllocator, OwnsSlotAlignment)
{
    SlabAllocator pool(64, 4);

    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);

    // 정상 슬롯은 owns() == true
    EXPECT_TRUE(pool.owns(p));

    // 비정렬 포인터는 owns() == false
    auto* misaligned = static_cast<uint8_t*>(p) + 1;
    EXPECT_FALSE(pool.owns(misaligned));

    // 풀 범위 밖 포인터는 owns() == false
    int outside;
    EXPECT_FALSE(pool.owns(&outside));

    pool.deallocate(p);
}

// --- CoreAllocator concept API tests ---

TEST(SlabAllocator, ConceptCompliance_AllocateOverload)
{
    SlabAllocator alloc(64, 8);
    void* p = alloc.allocate(64, 16); // size <= slot_size → 할당 성공
    ASSERT_NE(p, nullptr);
    alloc.deallocate(p);
}

TEST(SlabAllocator, AllocateOverload_SizeExceedsSlot)
{
    SlabAllocator alloc(32, 4);
    void* p = alloc.allocate(64, 8); // size > slot_size → nullptr
    EXPECT_EQ(p, nullptr);
}

TEST(SlabAllocator, UsedBytesAndCapacity)
{
    SlabAllocator alloc(32, 4);
    EXPECT_EQ(alloc.capacity(), 4 * alloc.slot_size());
    EXPECT_EQ(alloc.used_bytes(), 0u);
    void* p1 = alloc.allocate();
    void* p2 = alloc.allocate();
    EXPECT_EQ(alloc.used_bytes(), 2 * alloc.slot_size());
    alloc.deallocate(p1);
    EXPECT_EQ(alloc.used_bytes(), 1 * alloc.slot_size());
    alloc.deallocate(p2);
}

TEST(SlabAllocator, DoubleFreeCount)
{
    SlabAllocator alloc(64, 4);
    void* p = alloc.allocate();
    alloc.deallocate(p);
    EXPECT_EQ(alloc.double_free_count(), 0u);
    alloc.deallocate(p); // double-free
    EXPECT_EQ(alloc.double_free_count(), 1u);
    // free_count는 변하지 않아야 함
    EXPECT_EQ(alloc.free_count(), 4u); // 원래 4개 중 1개 할당 후 1개 반환 = 4
}
