#include <apex/core/arena_allocator.hpp>
#include <apex/core/core_allocator.hpp>
#include <gtest/gtest.h>
#include <cstdint>

using namespace apex::core;

// Concept 검증
static_assert(CoreAllocator<ArenaAllocator>);
static_assert(Resettable<ArenaAllocator>);
static_assert(!Freeable<ArenaAllocator>);

TEST(ArenaAllocator, ConstructWithDefaults) {
    ArenaAllocator alloc;  // default: block=4KB, max=1MB
    EXPECT_EQ(alloc.used_bytes(), 0);
    EXPECT_GT(alloc.capacity(), 0);  // 첫 블록 용량
}

TEST(ArenaAllocator, ConstructWithConfig) {
    ArenaAllocator alloc(8192, 65536);  // block=8KB, max=64KB
    EXPECT_EQ(alloc.capacity(), 8192);
}

TEST(ArenaAllocator, AllocateWithinSingleBlock) {
    ArenaAllocator alloc(4096, 1048576);
    void* p1 = alloc.allocate(100, 8);
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % 8, 0);
    void* p2 = alloc.allocate(200, 16);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 16, 0);
    EXPECT_GE(alloc.used_bytes(), 300);
}

TEST(ArenaAllocator, BlockChainingOnOverflow) {
    ArenaAllocator alloc(64, 1024);  // 작은 블록으로 체이닝 테스트
    void* p1 = alloc.allocate(60, 1);
    ASSERT_NE(p1, nullptr);
    // 두 번째 할당은 새 블록에서
    void* p2 = alloc.allocate(60, 1);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    // 두 포인터 모두 소유
    EXPECT_TRUE(alloc.owns(p1));
    EXPECT_TRUE(alloc.owns(p2));
}

TEST(ArenaAllocator, MaxBytesLimit) {
    ArenaAllocator alloc(64, 128);  // max=128바이트
    void* p1 = alloc.allocate(64, 1);
    ASSERT_NE(p1, nullptr);
    void* p2 = alloc.allocate(64, 1);
    ASSERT_NE(p2, nullptr);
    // 128바이트 초과 시 nullptr
    void* p3 = alloc.allocate(1, 1);
    EXPECT_EQ(p3, nullptr);
}

TEST(ArenaAllocator, ResetReleasesAllButFirstBlock) {
    ArenaAllocator alloc(64, 1024);
    alloc.allocate(60, 1);
    alloc.allocate(60, 1);  // 2nd block
    alloc.allocate(60, 1);  // 3rd block
    EXPECT_GE(alloc.used_bytes(), 180);
    alloc.reset();
    EXPECT_EQ(alloc.used_bytes(), 0);
    EXPECT_EQ(alloc.capacity(), 64);  // 첫 블록만 유지
    // 다시 할당 가능
    void* p = alloc.allocate(64, 1);
    ASSERT_NE(p, nullptr);
}

TEST(ArenaAllocator, OwnsCheckAcrossBlocks) {
    ArenaAllocator alloc(32, 1024);
    void* p1 = alloc.allocate(30, 1);
    void* p2 = alloc.allocate(30, 1);  // 새 블록
    EXPECT_TRUE(alloc.owns(p1));
    EXPECT_TRUE(alloc.owns(p2));
    int x = 0;
    EXPECT_FALSE(alloc.owns(&x));
    EXPECT_FALSE(alloc.owns(nullptr));
}

TEST(ArenaAllocator, AlignmentAcrossBlockBoundary) {
    ArenaAllocator alloc(48, 1024);
    alloc.allocate(47, 1);  // 블록 거의 가득
    // 다음 할당은 alignment 후 공간 부족 → 새 블록
    void* p = alloc.allocate(16, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0);
}

TEST(ArenaAllocator, LargeAllocationSpanningBlock) {
    ArenaAllocator alloc(64, 4096);
    // 블록보다 큰 할당은 전용 블록에 배치
    void* p = alloc.allocate(128, 1);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(alloc.owns(p));
}
