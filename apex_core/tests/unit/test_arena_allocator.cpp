// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/arena_allocator.hpp>
#include <apex/core/core_allocator.hpp>
#include <cstdint>
#include <gtest/gtest.h>

using namespace apex::core;

// Concept 검증
static_assert(CoreAllocator<ArenaAllocator>);
static_assert(Resettable<ArenaAllocator>);
static_assert(!Freeable<ArenaAllocator>);

TEST(ArenaAllocator, ConstructWithDefaults)
{
    ArenaAllocator alloc; // default: block=4KB, max=1MB
    EXPECT_EQ(alloc.used_bytes(), 0);
    EXPECT_GT(alloc.capacity(), 0); // 첫 블록 용량
}

TEST(ArenaAllocator, ConstructWithConfig)
{
    ArenaAllocator alloc(8192, 65536); // block=8KB, max=64KB
    EXPECT_EQ(alloc.capacity(), 8192);
}

TEST(ArenaAllocator, AllocateWithinSingleBlock)
{
    ArenaAllocator alloc(4096, 1048576);
    void* p1 = alloc.allocate(100, 8);
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % 8, 0);
    void* p2 = alloc.allocate(200, 16);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 16, 0);
    EXPECT_GE(alloc.used_bytes(), 300);
}

TEST(ArenaAllocator, BlockChainingOnOverflow)
{
    ArenaAllocator alloc(64, 1024); // 작은 블록으로 체이닝 테스트
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

TEST(ArenaAllocator, MaxBytesLimit)
{
    ArenaAllocator alloc(64, 128); // max=128바이트
    void* p1 = alloc.allocate(64, 1);
    ASSERT_NE(p1, nullptr);
    void* p2 = alloc.allocate(64, 1);
    ASSERT_NE(p2, nullptr);
    // 128바이트 초과 시 nullptr
    void* p3 = alloc.allocate(1, 1);
    EXPECT_EQ(p3, nullptr);
}

TEST(ArenaAllocator, ResetReleasesAllButFirstBlock)
{
    ArenaAllocator alloc(64, 1024);
    (void)alloc.allocate(60, 1);
    (void)alloc.allocate(60, 1); // 2nd block
    (void)alloc.allocate(60, 1); // 3rd block
    EXPECT_GE(alloc.used_bytes(), 180);
    alloc.reset();
    EXPECT_EQ(alloc.used_bytes(), 0);
    EXPECT_EQ(alloc.capacity(), 64); // 첫 블록만 유지
    // 다시 할당 가능
    void* p = alloc.allocate(64, 1);
    ASSERT_NE(p, nullptr);
}

TEST(ArenaAllocator, OwnsCheckAcrossBlocks)
{
    ArenaAllocator alloc(32, 1024);
    void* p1 = alloc.allocate(30, 1);
    void* p2 = alloc.allocate(30, 1); // 새 블록
    EXPECT_TRUE(alloc.owns(p1));
    EXPECT_TRUE(alloc.owns(p2));
    int x = 0;
    EXPECT_FALSE(alloc.owns(&x));
    EXPECT_FALSE(alloc.owns(nullptr));
}

TEST(ArenaAllocator, AlignmentAcrossBlockBoundary)
{
    ArenaAllocator alloc(48, 1024);
    (void)alloc.allocate(47, 1); // 블록 거의 가득
    // 다음 할당은 alignment 후 공간 부족 → 새 블록
    void* p = alloc.allocate(16, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0);
}

TEST(ArenaAllocator, LargeAllocationSpanningBlock)
{
    ArenaAllocator alloc(64, 4096);
    // 블록보다 큰 할당은 전용 블록에 배치
    void* p = alloc.allocate(128, 1);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(alloc.owns(p));
}

TEST(ArenaAllocator, ZeroSizeAllocateReturnsNullptr)
{
    ArenaAllocator alloc(256, 4096);
    auto before = alloc.used_bytes();
    void* p = alloc.allocate(0, 1);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(alloc.used_bytes(), before); // 사용량 변화 없음
}

TEST(ArenaAllocator, MoveConstruction)
{
    ArenaAllocator src(256, 4096);
    void* p = src.allocate(64, 1);
    ASSERT_NE(p, nullptr);

    ArenaAllocator dst(std::move(src));
    // moved-from 상태: owns()는 빈 blocks_ 순회 → false
    EXPECT_FALSE(src.owns(p));
    EXPECT_TRUE(dst.owns(p));
    EXPECT_EQ(dst.capacity(), 256);
    // 이동 후에도 정상 할당 가능
    void* p2 = dst.allocate(32, 1);
    ASSERT_NE(p2, nullptr);
}

TEST(ArenaAllocator, MoveAssignment)
{
    ArenaAllocator src(256, 4096);
    void* p = src.allocate(64, 1);
    ASSERT_NE(p, nullptr);

    ArenaAllocator dst(128, 4096);
    dst = std::move(src);
    EXPECT_FALSE(src.owns(p));
    EXPECT_TRUE(dst.owns(p));
    EXPECT_EQ(dst.capacity(), 256);
    // 이동 후에도 정상 할당 가능
    void* p2 = dst.allocate(32, 1);
    ASSERT_NE(p2, nullptr);
}

TEST(ArenaAllocator, InvalidAlignmentReturnsNullptr)
{
    ArenaAllocator alloc(256, 4096);
    // align=0
    void* p1 = alloc.allocate(16, 0);
    EXPECT_EQ(p1, nullptr);
    // align=5 (2의 거듭제곱 아님)
    void* p2 = alloc.allocate(16, 5);
    EXPECT_EQ(p2, nullptr);
    // 유효하지 않은 alignment로 사용량 변화 없음
    EXPECT_EQ(alloc.used_bytes(), 0);
}
