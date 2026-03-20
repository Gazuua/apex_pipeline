// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/mpsc_queue.hpp>
#include <benchmark/benchmark.h>
#include <thread>

using namespace apex::core;

// 1P1C: single producer-consumer throughput
static void BM_MpscQueue_1P1C(benchmark::State& state)
{
    const auto capacity = static_cast<size_t>(state.range(0));
    MpscQueue<uint64_t> queue(capacity);
    for (auto _ : state)
    {
        auto res = queue.enqueue(42);
        benchmark::DoNotOptimize(res);
        auto val = queue.dequeue();
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MpscQueue_1P1C)->Range(1024, 65536);

// 2P1C: 2 producer threads + 1 consumer (benchmark thread)
static void BM_MpscQueue_2P1C(benchmark::State& state)
{
    MpscQueue<uint64_t> queue(65536);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> produced{0};
    std::jthread producer([&] {
        uint64_t val = 0;
        while (running.load(std::memory_order_relaxed))
        {
            if (queue.enqueue(val++).has_value())
                produced.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (auto _ : state)
    {
        (void)queue.enqueue(99);
        while (auto val = queue.dequeue())
        {
            benchmark::DoNotOptimize(val);
        }
    }
    running.store(false);
    state.SetItemsProcessed(state.iterations() + produced.load());
}
BENCHMARK(BM_MpscQueue_2P1C);

// Backpressure: enqueue into full queue
static void BM_MpscQueue_Backpressure(benchmark::State& state)
{
    MpscQueue<uint64_t> queue(64);
    for (size_t i = 0; i < queue.capacity(); ++i)
        (void)queue.enqueue(i);
    for (auto _ : state)
    {
        auto res = queue.enqueue(999);
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_MpscQueue_Backpressure);
