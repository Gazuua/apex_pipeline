// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/message_dispatcher.hpp>
#include <apex/core/result.hpp>
#include <benchmark/benchmark.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <unordered_map>

using namespace apex::core;

static void BM_Dispatcher_Lookup(benchmark::State& state)
{
    auto num_handlers = static_cast<int>(state.range(0));
    boost::asio::io_context io_ctx;
    MessageDispatcher dispatcher;
    for (int i = 0; i < num_handlers; ++i)
    {
        dispatcher.register_handler(
            static_cast<uint32_t>(i),
            [](SessionPtr /*session*/, uint32_t /*msg_id*/,
               std::span<const uint8_t> /*payload*/) -> boost::asio::awaitable<Result<void>> { co_return ok(); });
    }
    uint32_t target_id = static_cast<uint32_t>(num_handlers / 2);
    for (auto _ : state)
    {
        bool has = dispatcher.has_handler(target_id);
        benchmark::DoNotOptimize(has);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatcher_Lookup)->Arg(10)->Arg(100)->Arg(1000);

// ---------------------------------------------------------------------------
// SessionManager-scale lookup comparison
// Simulates SessionManager::sessions_ access pattern — thousands to hundreds
// of thousands of concurrent sessions where cache locality matters.
// boost::unordered_flat_map vs std::unordered_map
// ---------------------------------------------------------------------------

static void BM_FlatMap_SessionLookup(benchmark::State& state)
{
    auto num_sessions = static_cast<size_t>(state.range(0));
    boost::unordered_flat_map<uint64_t, int> sessions;
    for (size_t i = 0; i < num_sessions; ++i)
        sessions.emplace(i * 7 + 13, static_cast<int>(i)); // scattered keys

    uint64_t key = (num_sessions / 2) * 7 + 13; // known existing key
    for (auto _ : state)
    {
        auto it = sessions.find(key);
        benchmark::DoNotOptimize(it);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FlatMap_SessionLookup)->Arg(100)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_StdMap_SessionLookup(benchmark::State& state)
{
    auto num_sessions = static_cast<size_t>(state.range(0));
    std::unordered_map<uint64_t, int> sessions;
    for (size_t i = 0; i < num_sessions; ++i)
        sessions.emplace(i * 7 + 13, static_cast<int>(i));

    uint64_t key = (num_sessions / 2) * 7 + 13;
    for (auto _ : state)
    {
        auto it = sessions.find(key);
        benchmark::DoNotOptimize(it);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdMap_SessionLookup)->Arg(100)->Arg(1000)->Arg(10000)->Arg(100000);

// ---------------------------------------------------------------------------
// Iteration benchmark — broadcasting to all sessions
// ---------------------------------------------------------------------------

static void BM_FlatMap_SessionIterate(benchmark::State& state)
{
    auto num_sessions = static_cast<size_t>(state.range(0));
    boost::unordered_flat_map<uint64_t, int> sessions;
    for (size_t i = 0; i < num_sessions; ++i)
        sessions.emplace(i * 7 + 13, static_cast<int>(i));

    for (auto _ : state)
    {
        int sum = 0;
        for (auto& [k, v] : sessions)
            sum += v;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_sessions));
}
BENCHMARK(BM_FlatMap_SessionIterate)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_StdMap_SessionIterate(benchmark::State& state)
{
    auto num_sessions = static_cast<size_t>(state.range(0));
    std::unordered_map<uint64_t, int> sessions;
    for (size_t i = 0; i < num_sessions; ++i)
        sessions.emplace(i * 7 + 13, static_cast<int>(i));

    for (auto _ : state)
    {
        int sum = 0;
        for (auto& [k, v] : sessions)
            sum += v;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_sessions));
}
BENCHMARK(BM_StdMap_SessionIterate)->Arg(100)->Arg(1000)->Arg(10000);
