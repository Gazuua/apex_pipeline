// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/wire_header.hpp>
#include <generated/echo_generated.h>

#include <benchmark/benchmark.h>
#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace apex::core;

// ---------------------------------------------------------------------------
// FlatBuffers: build EchoRequest with a data vector of given size
// ---------------------------------------------------------------------------

static void BM_FlatBuffers_Build(benchmark::State& state)
{
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz, 0xAB);

    for (auto _ : state)
    {
        flatbuffers::FlatBufferBuilder builder(sz + 64);
        auto data_vec = builder.CreateVector(payload);
        auto req = apex::messages::CreateEchoRequest(builder, data_vec);
        builder.Finish(req);
        benchmark::DoNotOptimize(builder.GetBufferPointer());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_FlatBuffers_Build)->Arg(64)->Arg(512)->Arg(4096);

// ---------------------------------------------------------------------------
// HeapAlloc: new uint8_t[] + memcpy to build equivalent raw payload
// ---------------------------------------------------------------------------

static void BM_HeapAlloc_Build(benchmark::State& state)
{
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz, 0xAB);

    for (auto _ : state)
    {
        auto* buf = new uint8_t[WireHeader::SIZE + sz];
        std::memcpy(buf + WireHeader::SIZE, payload.data(), sz);
        benchmark::DoNotOptimize(buf);
        benchmark::ClobberMemory();
        delete[] buf;
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_HeapAlloc_Build)->Arg(64)->Arg(512)->Arg(4096);

// ---------------------------------------------------------------------------
// READ benchmarks — 1회 빌드 후 N회 읽기 (FlatBuffers zero-copy 이점 측정)
// ---------------------------------------------------------------------------

static void BM_FlatBuffers_Read(benchmark::State& state)
{
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz, 0xAB);

    // Setup: build once
    flatbuffers::FlatBufferBuilder builder(sz + 64);
    auto data_vec = builder.CreateVector(payload);
    auto req = apex::messages::CreateEchoRequest(builder, data_vec);
    builder.Finish(req);

    const uint8_t* buf = builder.GetBufferPointer();

    for (auto _ : state)
    {
        // Zero-copy: GetRoot returns pointer into existing buffer, no memcpy
        auto* parsed = flatbuffers::GetRoot<apex::messages::EchoRequest>(buf);
        auto* data = parsed->data();
        benchmark::DoNotOptimize(data->data());
        benchmark::DoNotOptimize(data->size());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_FlatBuffers_Read)->Arg(64)->Arg(512)->Arg(4096);

static void BM_HeapAlloc_Read(benchmark::State& state)
{
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz, 0xAB);

    // Setup: build once (wire header + payload)
    auto total = WireHeader::SIZE + sz;
    std::vector<uint8_t> wire(total);
    std::memcpy(wire.data() + WireHeader::SIZE, payload.data(), sz);

    for (auto _ : state)
    {
        // Must copy payload out to "deserialize" — cannot access in-place safely
        auto* read_buf = new uint8_t[sz];
        std::memcpy(read_buf, wire.data() + WireHeader::SIZE, sz);
        benchmark::DoNotOptimize(read_buf);
        benchmark::ClobberMemory();
        delete[] read_buf;
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_HeapAlloc_Read)->Arg(64)->Arg(512)->Arg(4096);
