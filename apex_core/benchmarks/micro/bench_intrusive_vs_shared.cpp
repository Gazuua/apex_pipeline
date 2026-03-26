// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <atomic>
#include <benchmark/benchmark.h>
#include <boost/intrusive_ptr.hpp>
#include <cstdint>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Benchmark-local intrusive refcount object — mirrors Session's pattern
// (atomic refcount + intrusive_ptr_add_ref / intrusive_ptr_release ADL pair)
// ---------------------------------------------------------------------------

class IntrusiveObject
{
  public:
    IntrusiveObject() = default;
    ~IntrusiveObject() = default;

    IntrusiveObject(const IntrusiveObject&) = delete;
    IntrusiveObject& operator=(const IntrusiveObject&) = delete;

    [[nodiscard]] uint32_t refcount() const noexcept
    {
        return refcount_.load(std::memory_order_relaxed);
    }

    friend void intrusive_ptr_add_ref(IntrusiveObject* p) noexcept
    {
        p->refcount_.fetch_add(1, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_release(IntrusiveObject* p) noexcept
    {
        if (p->refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            delete p;
        }
    }

  private:
    std::atomic<uint32_t> refcount_{0};
};

// Shared-ptr counterpart — same payload weight (just an atomic<uint32_t> field)
struct SharedObject
{
    std::atomic<uint32_t> dummy_{0};
};

// ===========================================================================
// 1. SingleThread_RefcountThroughput
//    단일 스레드에서 copy-construct + destruct (refcount inc/dec) 반복
// ===========================================================================

static void BM_IntrusivePtr_SingleThread(benchmark::State& state)
{
    boost::intrusive_ptr<IntrusiveObject> base(new IntrusiveObject);
    for (auto _ : state)
    {
        auto copy = base;
        benchmark::DoNotOptimize(copy.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_IntrusivePtr_SingleThread);

static void BM_SharedPtr_SingleThread(benchmark::State& state)
{
    auto base = std::make_shared<SharedObject>();
    for (auto _ : state)
    {
        auto copy = base;
        benchmark::DoNotOptimize(copy.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SharedPtr_SingleThread);

// ===========================================================================
// 2. MultiThread_Contention
//    N 스레드가 동일 객체의 refcount를 경합 (copy + destruct)
//    ->Threads(1/2/4/8) 패턴으로 스레드 수별 확장성 측정
// ===========================================================================

static void BM_IntrusivePtr_Contention(benchmark::State& state)
{
    // 모든 스레드가 하나의 객체를 공유하기 위해 static + 첫 스레드만 생성
    static boost::intrusive_ptr<IntrusiveObject> shared_obj;
    if (state.thread_index() == 0)
    {
        shared_obj.reset(new IntrusiveObject);
    }

    for (auto _ : state)
    {
        auto copy = shared_obj;
        benchmark::DoNotOptimize(copy.get());
    }
    state.SetItemsProcessed(state.iterations());

    if (state.thread_index() == 0)
    {
        shared_obj.reset();
    }
}
BENCHMARK(BM_IntrusivePtr_Contention)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_SharedPtr_Contention(benchmark::State& state)
{
    static std::shared_ptr<SharedObject> shared_obj;
    if (state.thread_index() == 0)
    {
        shared_obj = std::make_shared<SharedObject>();
    }

    for (auto _ : state)
    {
        auto copy = shared_obj;
        benchmark::DoNotOptimize(copy.get());
    }
    state.SetItemsProcessed(state.iterations());

    if (state.thread_index() == 0)
    {
        shared_obj.reset();
    }
}
BENCHMARK(BM_SharedPtr_Contention)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ===========================================================================
// 3. SessionLike_Lifecycle
//    Session과 유사한 객체 생성 → N개 참조 공유 → 모두 해제 사이클
//    Arg(N) = 동시 참조 수
// ===========================================================================

static void BM_IntrusivePtr_Lifecycle(benchmark::State& state)
{
    const auto num_refs = static_cast<size_t>(state.range(0));
    for (auto _ : state)
    {
        // 새 객체 생성 (힙 할당 + refcount 0→1)
        boost::intrusive_ptr<IntrusiveObject> origin(new IntrusiveObject);

        // N개의 참조 공유 (refcount 1→N+1)
        std::vector<boost::intrusive_ptr<IntrusiveObject>> refs(num_refs, origin);
        benchmark::DoNotOptimize(refs.data());

        // 모든 참조 해제 (refcount N+1→0, delete)
        refs.clear();
        origin.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_IntrusivePtr_Lifecycle)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

static void BM_SharedPtr_Lifecycle(benchmark::State& state)
{
    const auto num_refs = static_cast<size_t>(state.range(0));
    for (auto _ : state)
    {
        // 새 객체 생성 (힙 할당 + control block)
        auto origin = std::make_shared<SharedObject>();

        // N개의 참조 공유 (strong count 1→N+1)
        std::vector<std::shared_ptr<SharedObject>> refs(num_refs, origin);
        benchmark::DoNotOptimize(refs.data());

        // 모든 참조 해제 (strong count N+1→0, delete)
        refs.clear();
        origin.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SharedPtr_Lifecycle)->Arg(4)->Arg(16)->Arg(64)->Arg(256);
