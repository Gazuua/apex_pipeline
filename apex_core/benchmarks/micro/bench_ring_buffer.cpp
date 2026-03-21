// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/ring_buffer.hpp>
#include <benchmark/benchmark.h>
#include <cstring>
#include <vector>

using namespace apex::core;

static void BM_RingBuffer_WriteRead(benchmark::State& state)
{
    auto payload_size = static_cast<size_t>(state.range(0));
    RingBuffer rb(payload_size * 4);
    std::vector<uint8_t> data(payload_size, 0xAB);
    for (auto _ : state)
    {
        rb.write(data);
        auto readable = rb.contiguous_read();
        benchmark::DoNotOptimize(readable.data());
        rb.consume(readable.size());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(payload_size));
}
BENCHMARK(BM_RingBuffer_WriteRead)->Range(64, 4096);

static void BM_RingBuffer_Linearize(benchmark::State& state)
{
    auto payload_size = static_cast<size_t>(state.range(0));
    RingBuffer rb(payload_size * 2);
    std::vector<uint8_t> data(payload_size, 0xAB);
    // Fill halfway to force wrap-around on second write
    rb.write(std::span<const uint8_t>(data.data(), payload_size / 2));
    rb.consume(payload_size / 2);
    for (auto _ : state)
    {
        rb.write(data);
        auto span = rb.linearize(payload_size);
        benchmark::DoNotOptimize(span.data());
        rb.consume(payload_size);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(payload_size));
}
BENCHMARK(BM_RingBuffer_Linearize)->Range(64, 4096);

// ---------------------------------------------------------------------------
// Naive buffer: allocate + copy + deallocate per iteration (baseline)
// ---------------------------------------------------------------------------

static void BM_NaiveBuffer_CopyWrite(benchmark::State& state)
{
    const auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> src(sz, 0xAB);

    for (auto _ : state)
    {
        auto* buf = new uint8_t[sz];
        std::memcpy(buf, src.data(), sz);
        benchmark::DoNotOptimize(buf);
        benchmark::ClobberMemory();
        delete[] buf;
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_NaiveBuffer_CopyWrite)->Arg(64)->Arg(512)->Arg(4096);
