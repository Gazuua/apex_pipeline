// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Architecture comparison — Stateful Message Processing
//
// Simulates realistic server workload: each handler looks up a session,
// reads state, computes, and writes back.
//
//   A) Per-core: each core owns its sessions — zero contention, linear scaling
//   B) Shared (sharded): one io_context, sharded session map with per-shard mutex
//      — the industry-standard optimization for shared-state servers
//
// Sharded mutex is a fair comparison: it's what a skilled engineer would
// build if they chose the traditional thread pool model.
// ---------------------------------------------------------------------------

static constexpr int SESSIONS_PER_CORE = 1000;
static constexpr int MSGS_PER_CORE = 50000;

struct SessionState
{
    uint64_t msg_count{0};
    uint64_t bytes_processed{0};
    uint64_t last_activity{0};
    std::array<uint64_t, 4> context{};

    void process(uint64_t payload)
    {
        ++msg_count;
        bytes_processed += payload & 0xFF;
        last_activity = payload;
        context[0] ^= payload;
        context[1] += context[0] >> 3;
        context[2] ^= context[1] << 5;
        context[3] += context[2];
    }
};

// Sharded session map — reduces contention by distributing locks
static constexpr int NUM_SHARDS = 64;

struct alignas(64) SessionShard
{
    std::unordered_map<uint64_t, SessionState> sessions;
    std::mutex mtx;
};

// A) Per-core: each core owns its sessions, post + run in same thread
static void BM_PerCore_Stateful(benchmark::State& state)
{
    const auto num_cores = static_cast<int>(state.range(0));
    const int total_msgs = num_cores * MSGS_PER_CORE;

    for (auto _ : state)
    {
        std::vector<std::unordered_map<uint64_t, SessionState>> per_core_sessions(num_cores);
        for (int c = 0; c < num_cores; ++c)
            for (int s = 0; s < SESSIONS_PER_CORE; ++s)
                per_core_sessions[c][static_cast<uint64_t>(s)];

        auto worker = [&per_core_sessions](int c) {
            boost::asio::io_context ctx;
            for (int i = 0; i < MSGS_PER_CORE; ++i)
            {
                auto sid = static_cast<uint64_t>(i % SESSIONS_PER_CORE);
                auto payload = static_cast<uint64_t>(i);
                boost::asio::post(
                    ctx, [&per_core_sessions, c, sid, payload]() { per_core_sessions[c][sid].process(payload); });
            }
            ctx.run();
        };

        std::vector<std::jthread> threads;
        threads.reserve(num_cores - 1);
        for (int c = 0; c < num_cores - 1; ++c)
            threads.emplace_back(worker, c);
        worker(num_cores - 1);
        for (auto& t : threads)
            t.join();

        uint64_t total = 0;
        for (int c = 0; c < num_cores; ++c)
            for (auto& kv : per_core_sessions[c])
                total += kv.second.msg_count;
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations() * total_msgs);
}
BENCHMARK(BM_PerCore_Stateful)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->Arg(8)->Arg(16)->UseRealTime();

// B) Shared (sharded): one io_context, sharded session map
//    — per-shard mutex reduces contention vs single global mutex
static void BM_Shared_Stateful(benchmark::State& state)
{
    const auto num_threads = static_cast<int>(state.range(0));
    const int total_msgs = num_threads * MSGS_PER_CORE;
    const int total_sessions = SESSIONS_PER_CORE * num_threads;

    for (auto _ : state)
    {
        boost::asio::io_context ctx;

        // Sharded session map — each shard has its own mutex
        std::array<SessionShard, NUM_SHARDS> shards;
        for (int s = 0; s < total_sessions; ++s)
            shards[s % NUM_SHARDS].sessions[static_cast<uint64_t>(s)];

        // Pre-post all messages
        for (int i = 0; i < total_msgs; ++i)
        {
            auto sid = static_cast<uint64_t>(i % total_sessions);
            auto payload = static_cast<uint64_t>(i);
            auto shard_idx = sid % NUM_SHARDS;
            boost::asio::post(ctx, [&shards, shard_idx, sid, payload]() {
                // Lock only the relevant shard — other shards are accessible concurrently
                std::lock_guard lock(shards[shard_idx].mtx);
                shards[shard_idx].sessions[sid].process(payload);
            });
        }

        // N-1 worker threads + benchmark thread
        std::vector<std::jthread> threads;
        threads.reserve(num_threads - 1);
        for (int t = 0; t < num_threads - 1; ++t)
            threads.emplace_back([&ctx]() { ctx.run(); });
        ctx.run();
        for (auto& t : threads)
            t.join();

        uint64_t total = 0;
        for (auto& shard : shards)
            for (auto& kv : shard.sessions)
                total += kv.second.msg_count;
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations() * total_msgs);
}
BENCHMARK(BM_Shared_Stateful)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->Arg(8)->Arg(16)->UseRealTime();

// C) Shared (concurrent_flat_map): one io_context, boost::concurrent_flat_map
//    — internally sharded with fine-grained locking, no external mutex needed
static void BM_Shared_LockFree_Stateful(benchmark::State& state)
{
    const auto num_threads = static_cast<int>(state.range(0));
    const int total_msgs = num_threads * MSGS_PER_CORE;
    const int total_sessions = SESSIONS_PER_CORE * num_threads;

    for (auto _ : state)
    {
        boost::asio::io_context ctx;

        // Lock-free concurrent session map — no external synchronization needed
        boost::concurrent_flat_map<uint64_t, SessionState> sessions;
        for (int s = 0; s < total_sessions; ++s)
            sessions.emplace(static_cast<uint64_t>(s), SessionState{});

        // Pre-post all messages
        for (int i = 0; i < total_msgs; ++i)
        {
            auto sid = static_cast<uint64_t>(i % total_sessions);
            auto payload = static_cast<uint64_t>(i);
            boost::asio::post(ctx, [&sessions, sid, payload]() {
                // In-place visitation — no operator[], lock-free internally
                sessions.visit(sid, [payload](auto& pair) { pair.second.process(payload); });
            });
        }

        // N-1 worker threads + benchmark thread
        std::vector<std::jthread> threads;
        threads.reserve(num_threads - 1);
        for (int t = 0; t < num_threads - 1; ++t)
            threads.emplace_back([&ctx]() { ctx.run(); });
        ctx.run();
        for (auto& t : threads)
            t.join();

        uint64_t total = 0;
        sessions.cvisit_all([&total](const auto& pair) { total += pair.second.msg_count; });
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations() * total_msgs);
}
BENCHMARK(BM_Shared_LockFree_Stateful)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->Arg(8)->Arg(16)->UseRealTime();
