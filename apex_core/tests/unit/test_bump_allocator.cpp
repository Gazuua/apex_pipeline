#include <apex/core/bump_allocator.hpp>
#include <apex/core/core_allocator.hpp>
#include <gtest/gtest.h>
#include <cstdint>

using namespace apex::core;

// Concept 만족 컴파일 타임 검증
static_assert(CoreAllocator<BumpAllocator>);
static_assert(Resettable<BumpAllocator>);
static_assert(!Freeable<BumpAllocator>);

TEST(BumpAllocator, ConstructWithCapacity) {
    BumpAllocator alloc(1024);
    EXPECT_EQ(alloc.capacity(), 1024);
    EXPECT_EQ(alloc.used_bytes(), 0);
}

TEST(BumpAllocator, AllocateReturnsAlignedPointer) {
    BumpAllocator alloc(1024);
    void* p = alloc.allocate(16, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0);
    EXPECT_EQ(alloc.used_bytes(), 16);
}

TEST(BumpAllocator, AllocateWithDefaultAlignment) {
    BumpAllocator alloc(1024);
    void* p = alloc.allocate(32);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0);
}

TEST(BumpAllocator, AllocateAlign1ForCharBuffer) {
    BumpAllocator alloc(64);
    void* p1 = alloc.allocate(7, 1);  // char 배열, align=1
    ASSERT_NE(p1, nullptr);
    void* p2 = alloc.allocate(3, 1);
    ASSERT_NE(p2, nullptr);
    // align=1이면 패딩 없이 연속 배치
    EXPECT_EQ(static_cast<char*>(p2) - static_cast<char*>(p1), 7);
    EXPECT_EQ(alloc.used_bytes(), 10);
}

TEST(BumpAllocator, AllocateMultipleWithPadding) {
    BumpAllocator alloc(256);
    void* p1 = alloc.allocate(1, 1);   // 1바이트, align=1
    void* p2 = alloc.allocate(8, 8);   // 8바이트, align=8
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 8, 0);
    // p2는 p1 이후 8-byte 경계에 정렬됨
    EXPECT_GE(static_cast<char*>(p2) - static_cast<char*>(p1), 1);
}

TEST(BumpAllocator, OverflowReturnsNullptr) {
    BumpAllocator alloc(32);
    void* p1 = alloc.allocate(32, 1);
    ASSERT_NE(p1, nullptr);
    void* p2 = alloc.allocate(1, 1);
    EXPECT_EQ(p2, nullptr);  // 용량 초과
}

TEST(BumpAllocator, ResetRestoresFull) {
    BumpAllocator alloc(128);
    alloc.allocate(100, 1);
    EXPECT_EQ(alloc.used_bytes(), 100);
    alloc.reset();
    EXPECT_EQ(alloc.used_bytes(), 0);
    // reset 후 다시 할당 가능
    void* p = alloc.allocate(128, 1);
    ASSERT_NE(p, nullptr);
}

TEST(BumpAllocator, OwnsReturnsTrueForOwnedPointers) {
    BumpAllocator alloc(256);
    void* p = alloc.allocate(64, 1);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(alloc.owns(p));
}

TEST(BumpAllocator, OwnsReturnsFalseForExternalPointers) {
    BumpAllocator alloc(256);
    int x = 42;
    EXPECT_FALSE(alloc.owns(&x));
    EXPECT_FALSE(alloc.owns(nullptr));
}

TEST(BumpAllocator, ZeroSizeAllocateReturnsNullptr) {
    BumpAllocator alloc(64);
    void* p = alloc.allocate(0, 1);
    EXPECT_EQ(p, nullptr);  // size=0은 항상 nullptr
    EXPECT_EQ(alloc.used_bytes(), 0);  // 사용량 변화 없음
}

TEST(BumpAllocator, LargeAlignment) {
    BumpAllocator alloc(4096);
    void* p = alloc.allocate(64, 64);  // 캐시라인 정렬
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0);
}
