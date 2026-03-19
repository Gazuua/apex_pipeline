#include <apex/core/slab_allocator.hpp>
#include <array>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdlib>
#include <memory>

using namespace apex::core;

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
