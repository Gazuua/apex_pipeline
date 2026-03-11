#include <apex/core/core_engine.hpp>
#include <benchmark/benchmark.h>
#include <atomic>
#include <chrono>

using namespace apex::core;

// Measures ping-pong latency between core 0 and core 1
static void BM_CrossCore_Latency(benchmark::State& state) {
    CoreEngineConfig config{.num_cores = 2, .mpsc_queue_capacity = 65536};
    CoreEngine engine(config);

    std::atomic<uint64_t> pong_count{0};
    std::atomic<uint64_t> pong_time_ns{0};

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        if (msg.op == CrossCoreOp::Custom) {
            if (core_id == 1) {
                // Pong back to core 0
                CoreMessage pong{.op = CrossCoreOp::Custom, .source_core = 1, .data = msg.data};
                (void)engine.post_to(0, pong);
            } else {
                // Core 0 received pong
                auto now = std::chrono::steady_clock::now().time_since_epoch().count();
                auto sent = static_cast<int64_t>(msg.data);
                pong_time_ns.store(static_cast<uint64_t>(now - sent), std::memory_order_relaxed);
                pong_count.fetch_add(1, std::memory_order_release);
            }
        }
    });

    engine.start();

    for (auto _ : state) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        CoreMessage ping{.op = CrossCoreOp::Custom, .source_core = 0,
                         .data = static_cast<uintptr_t>(now)};
        (void)engine.post_to(1, ping);

        auto expected = pong_count.load(std::memory_order_acquire) + 1;
        while (pong_count.load(std::memory_order_acquire) < expected) {
            std::this_thread::yield();
        }
    }

    auto rtt_ns = pong_time_ns.load();
    state.counters["rtt_ns"] = benchmark::Counter(static_cast<double>(rtt_ns));
    state.counters["one_way_ns"] = benchmark::Counter(static_cast<double>(rtt_ns / 2));

    engine.stop();
    engine.join();
    engine.drain_remaining();
}
BENCHMARK(BM_CrossCore_Latency)->Iterations(10000)->UseRealTime();
