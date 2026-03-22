// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/arena_allocator.hpp>
#include <apex/core/bump_allocator.hpp>
#include <apex/core/slab_allocator.hpp>
#include <array>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>

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
// Args: alloc_size x capacity
// ---------------------------------------------------------------------------

static void BM_BumpAllocator_Alloc(benchmark::State& state)
{
    auto alloc_size = static_cast<size_t>(state.range(0));
    auto capacity = static_cast<size_t>(state.range(1));
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
BENCHMARK(BM_BumpAllocator_Alloc)
    ->Args({64, 16 * 1024})
    ->Args({64, 64 * 1024})
    ->Args({64, 256 * 1024})
    ->Args({256, 16 * 1024})
    ->Args({256, 64 * 1024})
    ->Args({256, 256 * 1024})
    ->Args({1024, 16 * 1024})
    ->Args({1024, 64 * 1024})
    ->Args({1024, 256 * 1024});

// ---------------------------------------------------------------------------
// Arena allocator: alloc-only monotonic (block-based), batch reset
// Args: alloc_size x block_size
// ---------------------------------------------------------------------------

static void BM_ArenaAllocator_Alloc(benchmark::State& state)
{
    auto alloc_size = static_cast<size_t>(state.range(0));
    auto block_size = static_cast<size_t>(state.range(1));
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
BENCHMARK(BM_ArenaAllocator_Alloc)
    ->Args({64, 1024})
    ->Args({64, 4 * 1024})
    ->Args({64, 16 * 1024})
    ->Args({256, 1024})
    ->Args({256, 4 * 1024})
    ->Args({256, 16 * 1024})
    ->Args({1024, 1024})
    ->Args({1024, 4 * 1024})
    ->Args({1024, 16 * 1024});

// ---------------------------------------------------------------------------
// Bump allocator: request cycle — variable-size allocs then reset
// Simulates a request processing cycle: 3-8 allocations of 32-512B, then reset.
// ---------------------------------------------------------------------------

static void BM_BumpAllocator_RequestCycle(benchmark::State& state)
{
    auto capacity = static_cast<size_t>(state.range(0));
    BumpAllocator alloc(capacity);
    std::mt19937 rng(42); // fixed seed for reproducibility
    std::uniform_int_distribution<size_t> alloc_count_dist(3, 8);
    std::uniform_int_distribution<size_t> alloc_size_dist(32, 512);

    for (auto _ : state)
    {
        size_t num_allocs = alloc_count_dist(rng);
        for (size_t i = 0; i < num_allocs; ++i)
        {
            size_t sz = alloc_size_dist(rng);
            void* p = alloc.allocate(sz);
            benchmark::DoNotOptimize(p);
        }
        alloc.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BumpAllocator_RequestCycle)->Arg(16 * 1024)->Arg(64 * 1024)->Arg(256 * 1024);

// ---------------------------------------------------------------------------
// Arena allocator: transaction cycle — variable-size allocs then reset
// Simulates a transaction processing cycle: 4-12 allocations of 128-2048B, then reset.
// ---------------------------------------------------------------------------

static void BM_ArenaAllocator_TransactionCycle(benchmark::State& state)
{
    auto block_size = static_cast<size_t>(state.range(0));
    constexpr size_t max_bytes = 1024 * 1024; // 1 MB total
    ArenaAllocator alloc(block_size, max_bytes);
    std::mt19937 rng(42); // fixed seed for reproducibility
    std::uniform_int_distribution<size_t> alloc_count_dist(4, 12);
    std::uniform_int_distribution<size_t> alloc_size_dist(128, 2048);

    for (auto _ : state)
    {
        size_t num_allocs = alloc_count_dist(rng);
        for (size_t i = 0; i < num_allocs; ++i)
        {
            size_t sz = alloc_size_dist(rng);
            void* p = alloc.allocate(sz);
            benchmark::DoNotOptimize(p);
        }
        alloc.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArenaAllocator_TransactionCycle)->Arg(1024)->Arg(4 * 1024)->Arg(16 * 1024);
