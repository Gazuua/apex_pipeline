// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/arena_allocator.hpp>
#include <apex/core/bump_allocator.hpp>
#include <apex/core/slab_allocator.hpp>
#include <array>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdlib>
#include <memory>

using namespace apex::core;

// ---------------------------------------------------------------------------
// Slab allocator: alloc + dealloc per iteration (fixed-size slot pool)
// ---------------------------------------------------------------------------

static void BM_SlabAllocator_AllocDealloc(benchmark::State& state)
{
    auto slot_size = static_cast<size_t>(state.range(0));
    SlabAllocator pool(slot_size, 1024);
    for (auto _ : state)
    {
        void* p = pool.allocate();
        benchmark::DoNotOptimize(p);
        pool.deallocate(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SlabAllocator_AllocDealloc)->Arg(64)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// malloc/free baseline
// ---------------------------------------------------------------------------

static void BM_Malloc_AllocFree(benchmark::State& state)
{
    auto slot_size = static_cast<size_t>(state.range(0));
    for (auto _ : state)
    {
        void* p = std::malloc(slot_size);
        benchmark::DoNotOptimize(p);
        std::free(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Malloc_AllocFree)->Arg(64)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// std::make_shared baseline (atomic refcount + heap alloc)
// ---------------------------------------------------------------------------

static void BM_MakeShared_AllocDealloc(benchmark::State& state)
{
    for (auto _ : state)
    {
        auto p = std::make_shared<std::array<uint8_t, 256>>();
        benchmark::DoNotOptimize(p.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MakeShared_AllocDealloc);

// ---------------------------------------------------------------------------
// Bump allocator: alloc-only monotonic, batch reset when nearing capacity
// ---------------------------------------------------------------------------

static void BM_BumpAllocator_Alloc(benchmark::State& state)
{
    auto alloc_size = static_cast<size_t>(state.range(0));
    constexpr size_t capacity = 1024 * 1024; // 1 MB arena
    BumpAllocator alloc(capacity);

    for (auto _ : state)
    {
        if (alloc.used_bytes() + alloc_size > alloc.capacity())
        {
            alloc.reset();
        }
        void* p = alloc.allocate(alloc_size);
        benchmark::DoNotOptimize(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BumpAllocator_Alloc)->Arg(64)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// Arena allocator: alloc-only monotonic (block-based), batch reset
// ---------------------------------------------------------------------------

static void BM_ArenaAllocator_Alloc(benchmark::State& state)
{
    auto alloc_size = static_cast<size_t>(state.range(0));
    constexpr size_t block_size = 64 * 1024;  // 64 KB per block
    constexpr size_t max_bytes = 1024 * 1024; // 1 MB total
    ArenaAllocator alloc(block_size, max_bytes);

    for (auto _ : state)
    {
        if (alloc.used_bytes() + alloc_size > alloc.capacity())
        {
            alloc.reset();
        }
        void* p = alloc.allocate(alloc_size);
        benchmark::DoNotOptimize(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArenaAllocator_Alloc)->Arg(64)->Arg(256)->Arg(1024);
