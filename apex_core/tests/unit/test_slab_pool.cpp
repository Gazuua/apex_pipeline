#include <apex/core/slab_pool.hpp>
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>
#include <set>

using namespace apex::core;

TEST(SlabPool, Construction) {
    SlabPool pool(64, 100);
    EXPECT_EQ(pool.total_count(), 100u);
    EXPECT_EQ(pool.free_count(), 100u);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_GE(pool.slot_size(), 64u);
}

TEST(SlabPool, AllocateAndDeallocate) {
    SlabPool pool(32, 10);
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.allocated_count(), 1u);
    EXPECT_EQ(pool.free_count(), 9u);

    pool.deallocate(p);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_EQ(pool.free_count(), 10u);
}

TEST(SlabPool, AllocateAll) {
    SlabPool pool(16, 5);
    std::vector<void*> ptrs;
    for (int i = 0; i < 5; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.free_count(), 0u);

    // Pool exhausted
    EXPECT_EQ(pool.allocate(), nullptr);

    // Deallocate all
    for (void* p : ptrs) pool.deallocate(p);
    EXPECT_EQ(pool.free_count(), 5u);
}

TEST(SlabPool, NoDuplicatePointers) {
    SlabPool pool(64, 100);
    std::set<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(ptrs.insert(p).second) << "Duplicate pointer returned";
    }
}

TEST(SlabPool, ReuseAfterDeallocate) {
    SlabPool pool(32, 2);
    void* p1 = pool.allocate();
    pool.deallocate(p1);
    void* p2 = pool.allocate();
    EXPECT_EQ(p1, p2);  // LIFO free-list reuses same slot
}

// --- TypedSlabPool ---

struct TestObj {
    int x;
    float y;
    TestObj(int x, float y) : x(x), y(y) {}
};

TEST(SlabPool, ZeroInitialCountThrows) {
    EXPECT_THROW(SlabPool(64, 0), std::invalid_argument);
}

TEST(TypedSlabPool, ConstructAndDestroy) {
    TypedSlabPool<TestObj> pool(10);
    TestObj* obj = pool.construct(42, 3.14f);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->x, 42);
    EXPECT_FLOAT_EQ(obj->y, 3.14f);
    EXPECT_EQ(pool.allocated_count(), 1u);

    pool.destroy(obj);
    EXPECT_EQ(pool.allocated_count(), 0u);
}

// Double-free 감지 테스트
// Debug 빌드: assert로 즉시 포착 (EXPECT_DEATH)
// Release 빌드: silent return으로 corruption 방지
#ifdef NDEBUG
TEST(SlabPool, DoubleFreeProtection_Release) {
    SlabPool pool(64, 4);

    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.allocated_count(), 2u);

    // 정상 deallocate
    pool.deallocate(p1);
    EXPECT_EQ(pool.allocated_count(), 1u);
    EXPECT_EQ(pool.free_count(), 3u);

    // Double-free — Release에서는 silent return, 카운트 변경 없어야 함
    pool.deallocate(p1);
    EXPECT_EQ(pool.allocated_count(), 1u);  // 변경 없음
    EXPECT_EQ(pool.free_count(), 3u);       // 변경 없음

    // 나머지 정상 deallocate는 여전히 동작해야 함
    pool.deallocate(p2);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_EQ(pool.free_count(), 4u);

    // 풀이 여전히 정상 동작하는지 확인
    void* p3 = pool.allocate();
    EXPECT_NE(p3, nullptr);
    EXPECT_EQ(pool.allocated_count(), 1u);
}
#else
TEST(SlabPool, DoubleFreeProtection_Debug) {
    SlabPool pool(64, 4);

    void* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);

    pool.deallocate(p1);
    EXPECT_EQ(pool.free_count(), 4u);

    // Debug 빌드에서 double-free는 assert로 프로세스 종료
    // NOTE: MSVC assert 메시지 포맷: "Assertion failed: <expr>, file <f>, line <n>"
    // "double-free detected"는 assert 표현식의 일부이므로 MSVC에서도 매칭됨.
    EXPECT_DEATH(pool.deallocate(p1), "double-free detected");
}
#endif

// owns()가 슬롯 정렬을 검증하는지 확인
TEST(SlabPool, OwnsSlotAlignment) {
    SlabPool pool(64, 4);

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
