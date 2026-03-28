// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_queue.hpp>
#include <benchmark/benchmark.h>
#include <thread>

using namespace apex::core;

// 1P1C: single producer-consumer throughput (vs MpscQueue 1P1C)
static void BM_SpscQueue_Throughput(benchmark::State& state)
{
    const auto capacity = static_cast<size_t>(state.range(0));
    boost::asio::io_context io_ctx{1};
    SpscQueue<uint64_t> queue(capacity, io_ctx);
    for (auto _ : state)
    {
        auto res = queue.try_enqueue(42);
        benchmark::DoNotOptimize(res);
        auto val = queue.try_dequeue();
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscQueue_Throughput)->Range(1024, 65536);

// Latency: enqueue→dequeue one-way delay
static void BM_SpscQueue_Latency(benchmark::State& state)
{
    boost::asio::io_context io_ctx{1};
    SpscQueue<uint64_t> queue(1024, io_ctx);
    uint64_t val = 0;
    for (auto _ : state)
    {
        queue.try_enqueue(val++);
        auto out = queue.try_dequeue();
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscQueue_Latency);

// Backpressure: try_enqueue into full queue
static void BM_SpscQueue_Backpressure(benchmark::State& state)
{
    boost::asio::io_context io_ctx{1};
    SpscQueue<uint64_t> queue(64, io_ctx);
    for (size_t i = 0; i < queue.capacity(); ++i)
        queue.try_enqueue(i);
    for (auto _ : state)
    {
        auto res = queue.try_enqueue(999);
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_SpscQueue_Backpressure);

// Concurrent: producer and consumer on separate threads.
// Each iteration: 1 enqueue (spin until space) = deterministic per-iteration cost.
// Consumer drains on background thread, creating realistic contention.
static void BM_SpscQueue_ConcurrentThroughput(benchmark::State& state)
{
    boost::asio::io_context io_ctx{1};
    SpscQueue<uint64_t> queue(65536, io_ctx);
    std::atomic<bool> running{true};

    std::jthread consumer([&] {
        while (running.load(std::memory_order_relaxed))
        {
            if (auto val = queue.try_dequeue())
            {
                benchmark::DoNotOptimize(val);
            }
        }
        while (auto val = queue.try_dequeue())
        {
            benchmark::DoNotOptimize(val);
        }
    });

    uint64_t val = 0;
    for (auto _ : state)
    {
        while (!queue.try_enqueue(val))
        {
        }
        ++val;
    }

    running.store(false);
    consumer.join();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscQueue_ConcurrentThroughput)->Repetitions(5)->ReportAggregatesOnly(true);
