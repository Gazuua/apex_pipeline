#include <apex/core/timing_wheel.hpp>
#include <benchmark/benchmark.h>

using namespace apex::core;

static void BM_TimingWheel_ScheduleTick(benchmark::State& state) {
    auto num_entries = static_cast<size_t>(state.range(0));
    TimingWheel wheel(num_entries * 2, [](TimingWheel::EntryId) {});
    for (auto _ : state) {
        state.PauseTiming();
        for (size_t i = 0; i < num_entries; ++i) {
            (void)wheel.schedule(1);
        }
        state.ResumeTiming();
        wheel.tick();
        benchmark::DoNotOptimize(wheel.active_count());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_entries));
}
BENCHMARK(BM_TimingWheel_ScheduleTick)->Arg(1000)->Arg(10000)->Arg(50000);

static void BM_TimingWheel_ScheduleOnly(benchmark::State& state) {
    TimingWheel wheel(65536, [](TimingWheel::EntryId) {});
    for (auto _ : state) {
        auto id = wheel.schedule(100);
        benchmark::DoNotOptimize(id);
        wheel.cancel(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TimingWheel_ScheduleOnly);
