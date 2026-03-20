// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_engine.hpp>
#include <atomic>
#include <benchmark/benchmark.h>

using namespace apex::core;

// Measures throughput of inter-core message posting
static void BM_CrossCore_PostThroughput(benchmark::State& state)
{
    CoreEngineConfig config{.num_cores = 2,
                            .spsc_queue_capacity = 65536,
                            .tick_interval = std::chrono::milliseconds{100},
                            .drain_batch_limit = 1024};
    CoreEngine engine(config);

    std::atomic<uint64_t> received{0};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        if (msg.op == CrossCoreOp::Custom)
        {
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    engine.start();

    for (auto _ : state)
    {
        CoreMessage msg{.op = CrossCoreOp::Custom, .source_core = 0, .data = 42};
        auto result = engine.post_to(1, msg);
        benchmark::DoNotOptimize(result);
    }

    engine.stop();
    engine.join();
    engine.drain_remaining();

    state.SetItemsProcessed(state.iterations());
    state.counters["received"] = benchmark::Counter(static_cast<double>(received.load()));
}
BENCHMARK(BM_CrossCore_PostThroughput)->UseRealTime();
