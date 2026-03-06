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
