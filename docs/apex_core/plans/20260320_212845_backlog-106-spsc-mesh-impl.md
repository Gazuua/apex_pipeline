# BACKLOG-106: SPSC All-to-All Mesh 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CoreEngine의 MPSC inbox를 SPSC all-to-all mesh로 교체하여 CAS contention을 제거하고, awaitable backpressure를 도입한다.

**Architecture:** N×(N-1) SpscQueue mesh가 MpscQueue inbox를 대체. 코어→코어 통신은 awaitable SPSC를 사용하고, 비코어 스레드(Redis/Kafka)는 asio::post() 직접 사용으로 전환. async_initiate 기반 backpressure로 큐 full 시 코루틴 suspend/resume.

**Tech Stack:** C++23, Boost.Asio (awaitable, async_initiate), Google Test, Google Benchmark

**Spec:** `docs/apex_core/plans/20260320_211101_backlog-106-spsc-mesh-design.md`

---

## 파일 구조

### 신규 파일

| 파일 | 역할 |
|------|------|
| `apex_core/include/apex/core/spsc_queue.hpp` | SpscQueue\<T\> header-only 구현 |
| `apex_core/include/apex/core/spsc_mesh.hpp` | SpscMesh 클래스 선언 |
| `apex_core/src/spsc_mesh.cpp` | SpscMesh 구현 |
| `apex_core/tests/unit/test_spsc_queue.cpp` | SpscQueue 단위 테스트 |
| `apex_core/tests/unit/test_spsc_mesh.cpp` | SpscMesh 단위 테스트 |
| `apex_core/benchmarks/micro/bench_spsc_queue.cpp` | SpscQueue 마이크로 벤치마크 |
| `apex_core/benchmarks/integration/bench_spsc_mesh_contention.cpp` | Mesh contention 벤치마크 |

### 수정 파일

| 파일 | 변경 |
|------|------|
| `apex_core/CMakeLists.txt` | spsc_mesh.cpp 소스 추가 |
| `apex_core/tests/unit/CMakeLists.txt` | test_spsc_queue, test_spsc_mesh 등록 |
| `apex_core/benchmarks/CMakeLists.txt` | bench_spsc_queue, bench_spsc_mesh_contention 등록 |
| `apex_core/include/apex/core/core_engine.hpp` | CoreContext inbox 제거 + 생성자 변경, SpscMesh 추가, post_to() awaitable, broadcast() 수정 |
| `apex_core/src/core_engine.cpp` | SpscMesh 초기화, drain/schedule_drain 전환, 셧다운 |
| `apex_core/include/apex/core/cross_core_call.hpp` | API awaitable 전환 |
| `apex_core/include/apex/core/server.hpp` | Server::cross_core_post() awaitable 전환 |
| `apex_services/gateway/src/broadcast_fanout.cpp` | cross_core_post → asio::post 전환 |
| `apex_core/tests/unit/test_cross_core_call.cpp` | 테스트 awaitable 전환 (비코어 스레드 테스트 재설계 포함) |
| `apex_core/tests/unit/test_core_engine.cpp` | CoreEngine 테스트 갱신 (broadcast 자기코어 제외) |
| `apex_core/benchmarks/integration/bench_cross_core_latency.cpp` | SPSC 시나리오 추가 |
| `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp` | SPSC 시나리오 추가 |

---

## Task 1: SpscQueue\<T\> 기본 구현 (try_enqueue / try_dequeue)

await 없는 기본 SPSC 큐. MpscQueue를 참고하되 CAS를 제거한 wait-free 구조.

**Files:**
- Create: `apex_core/include/apex/core/spsc_queue.hpp`
- Reference: `apex_core/include/apex/core/mpsc_queue.hpp`
- Reference: `apex_core/include/apex/core/detail/math_utils.hpp` (next_power_of_2)

- [ ] **Step 1: 스켈레톤 작성**

`spsc_queue.hpp` 생성. 저작권 헤더 + pragma once + namespace. 클래스 선언만:

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/detail/math_utils.hpp>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace apex::core
{

/// Wait-free bounded SPSC (Single-Producer, Single-Consumer) queue.
/// Designed for inter-core communication in SPSC all-to-all mesh.
///
/// - Single producer enqueues (wait-free, no CAS).
/// - Single consumer dequeues (wait-free).
/// - Fixed capacity set at construction time (power-of-2).
/// - Cache-line aligned to prevent false sharing.
/// - Awaitable enqueue() for backpressure (큐 full → coroutine suspend).
///
/// Template parameter T must be trivially copyable.
///
/// ## Ordering guarantee
/// Strict FIFO — single producer, single consumer.
///
/// ## Thread safety
/// Exactly one producer thread and one consumer thread.
/// NOT safe for multiple producers or multiple consumers.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class alignas(64) SpscQueue
{
  public:
    explicit SpscQueue(size_t capacity, boost::asio::io_context& producer_io);
    ~SpscQueue();

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    // === Producer API (단일 스레드) ===

    /// Non-blocking enqueue. Returns false if queue is full.
    bool try_enqueue(const T& item) noexcept;

    // === Consumer API (단일 스레드) ===

    /// Non-blocking dequeue. Returns nullopt if queue is empty.
    [[nodiscard]] std::optional<T> try_dequeue() noexcept;

    /// Batch drain into caller-provided buffer. Returns count drained.
    size_t drain(std::span<T> batch) noexcept;

    /// Thread-safe approximate count.
    [[nodiscard]] size_t size_approx() const noexcept;

    /// Thread-safe.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Thread-safe approximate check.
    [[nodiscard]] bool empty() const noexcept;

  private:
    struct Slot
    {
        T data;
    };

    // Producer-only — 클래스 alignas(64)에 의해 캐시라인 선두 정렬
    alignas(64) size_t head_{0};

    // Consumer-only
    alignas(64) size_t tail_{0};

    // Cross-thread coordination (acquire-release만 사용)
    alignas(64) std::atomic<size_t> published_{0}; // producer writes (release), consumer reads (acquire)
    alignas(64) std::atomic<size_t> consumed_{0};  // consumer writes (release), producer reads (acquire)

    // Immutable after construction
    size_t capacity_;
    size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    // Await 지원 (Task 3에서 구현)
    boost::asio::io_context& producer_io_;
};
```

- [ ] **Step 2: 생성자 / 소멸자 구현**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
SpscQueue<T>::SpscQueue(size_t capacity, boost::asio::io_context& producer_io)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
    , slots_(std::make_unique<Slot[]>(capacity_))
    , producer_io_(producer_io)
{
    if (capacity_ == 0)
    {
        throw std::overflow_error("SpscQueue capacity overflow in next_power_of_2");
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
SpscQueue<T>::~SpscQueue() = default;
```

- [ ] **Step 3: try_enqueue / try_dequeue / drain 구현**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
bool SpscQueue<T>::try_enqueue(const T& item) noexcept
{
    auto consumed = consumed_.load(std::memory_order_acquire);
    if (head_ - consumed >= capacity_)
    {
        return false; // full
    }
    slots_[head_ & mask_].data = item;
    ++head_;
    published_.store(head_, std::memory_order_release);
    return true;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::optional<T> SpscQueue<T>::try_dequeue() noexcept
{
    auto published = published_.load(std::memory_order_acquire);
    if (tail_ >= published)
    {
        return std::nullopt; // empty
    }
    T item = slots_[tail_ & mask_].data;
    ++tail_;
    consumed_.store(tail_, std::memory_order_release);
    return item;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t SpscQueue<T>::drain(std::span<T> batch) noexcept
{
    auto published = published_.load(std::memory_order_acquire);
    size_t available = published - tail_;
    size_t count = std::min(available, batch.size());

    for (size_t i = 0; i < count; ++i)
    {
        batch[i] = slots_[(tail_ + i) & mask_].data;
    }
    tail_ += count;
    if (count > 0)
    {
        consumed_.store(tail_, std::memory_order_release);
    }
    return count;
}
```

- [ ] **Step 4: size_approx / capacity / empty 구현**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t SpscQueue<T>::size_approx() const noexcept
{
    return published_.load(std::memory_order_relaxed) -
           consumed_.load(std::memory_order_relaxed);
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t SpscQueue<T>::capacity() const noexcept
{
    return capacity_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool SpscQueue<T>::empty() const noexcept
{
    return published_.load(std::memory_order_relaxed) ==
           consumed_.load(std::memory_order_relaxed);
}
```

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/spsc_queue.hpp
git commit -m "feat(core): BACKLOG-106 SpscQueue<T> 기본 구현 (try_enqueue/try_dequeue/drain)"
```

---

## Task 2: SpscQueue 기본 단위 테스트

**Files:**
- Create: `apex_core/tests/unit/test_spsc_queue.cpp`
- Modify: `apex_core/tests/unit/CMakeLists.txt`

- [ ] **Step 1: CMake 등록**

`apex_core/tests/unit/CMakeLists.txt`에 추가 (크로스코어 인프라 테스트 섹션 아래):

```cmake
# SPSC 큐 테스트
apex_add_unit_test(test_spsc_queue test_spsc_queue.cpp)
```

`set_tests_properties`의 타임아웃 목록에 `test_spsc_queue` 추가.

- [ ] **Step 2: 기본 FIFO 테스트 작성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <array>
#include <thread>

namespace apex::core
{

class SpscQueueTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_ctx_{1};
};

TEST_F(SpscQueueTest, EnqueueDequeue_FIFO)
{
    SpscQueue<int> q(4, io_ctx_);

    EXPECT_TRUE(q.try_enqueue(10));
    EXPECT_TRUE(q.try_enqueue(20));
    EXPECT_TRUE(q.try_enqueue(30));

    auto v1 = q.try_dequeue();
    auto v2 = q.try_dequeue();
    auto v3 = q.try_dequeue();

    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v3.has_value());

    EXPECT_EQ(*v1, 10);
    EXPECT_EQ(*v2, 20);
    EXPECT_EQ(*v3, 30);
}

TEST_F(SpscQueueTest, DequeueEmpty_ReturnsNullopt)
{
    SpscQueue<int> q(4, io_ctx_);
    EXPECT_FALSE(q.try_dequeue().has_value());
}

TEST_F(SpscQueueTest, EnqueueFull_ReturnsFalse)
{
    SpscQueue<int> q(2, io_ctx_); // power-of-2 → capacity=2
    EXPECT_TRUE(q.try_enqueue(1));
    EXPECT_TRUE(q.try_enqueue(2));
    EXPECT_FALSE(q.try_enqueue(3)); // full
}

TEST_F(SpscQueueTest, CapacityRoundsUpToPowerOf2)
{
    SpscQueue<int> q(3, io_ctx_);
    EXPECT_EQ(q.capacity(), 4u);
}

TEST_F(SpscQueueTest, WrapAround)
{
    SpscQueue<int> q(4, io_ctx_);
    // Fill and drain multiple times to exercise wrap-around
    for (int round = 0; round < 3; ++round)
    {
        for (int i = 0; i < 4; ++i)
            EXPECT_TRUE(q.try_enqueue(round * 10 + i));
        EXPECT_FALSE(q.try_enqueue(999)); // full
        for (int i = 0; i < 4; ++i)
        {
            auto v = q.try_dequeue();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(*v, round * 10 + i);
        }
        EXPECT_TRUE(q.empty());
    }
}

TEST_F(SpscQueueTest, SizeApprox)
{
    SpscQueue<int> q(8, io_ctx_);
    EXPECT_EQ(q.size_approx(), 0u);
    q.try_enqueue(1);
    q.try_enqueue(2);
    EXPECT_EQ(q.size_approx(), 2u);
    q.try_dequeue();
    EXPECT_EQ(q.size_approx(), 1u);
}

TEST_F(SpscQueueTest, DrainBatch)
{
    SpscQueue<int> q(8, io_ctx_);
    for (int i = 0; i < 5; ++i)
        q.try_enqueue(i);

    std::array<int, 8> batch{};
    size_t count = q.drain(batch);
    EXPECT_EQ(count, 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(batch[i], i);

    EXPECT_TRUE(q.empty());
}

TEST_F(SpscQueueTest, CoreMessage_TrivialCopy)
{
    // CoreMessage와 동일한 크기의 trivially copyable struct
    struct Msg
    {
        uint16_t op;
        uint32_t src;
        uintptr_t data;
    };
    static_assert(std::is_trivially_copyable_v<Msg>);

    SpscQueue<Msg> q(4, io_ctx_);
    EXPECT_TRUE(q.try_enqueue({1, 2, 0x1234}));
    auto msg = q.try_dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->op, 1);
    EXPECT_EQ(msg->src, 2u);
    EXPECT_EQ(msg->data, 0x1234u);
}

TEST_F(SpscQueueTest, ConcurrentProducerConsumer_TSAN)
{
    // TSAN: producer와 consumer가 별도 스레드에서 동시 동작
    constexpr int COUNT = 10000;
    SpscQueue<int> q(1024, io_ctx_);

    std::thread producer([&] {
        for (int i = 0; i < COUNT; ++i)
        {
            while (!q.try_enqueue(i))
            {
                std::this_thread::yield();
            }
        }
    });

    int received = 0;
    int last = -1;
    while (received < COUNT)
    {
        auto v = q.try_dequeue();
        if (v)
        {
            EXPECT_EQ(*v, last + 1) << "FIFO violation at index " << received;
            last = *v;
            ++received;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    producer.join();
}

} // namespace apex::core
```

- [ ] **Step 3: 빌드 및 테스트 실행**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug --target test_spsc_queue
```

테스트 실행:
```bash
apex_core/bin/debug/test_spsc_queue.exe
```

Expected: 전 테스트 PASS.

- [ ] **Step 4: 커밋**

```bash
git add apex_core/tests/unit/test_spsc_queue.cpp apex_core/tests/unit/CMakeLists.txt
git commit -m "test(core): BACKLOG-106 SpscQueue 기본 단위 테스트"
```

---

## Task 3: SpscQueue Awaitable enqueue + cancel

**Files:**
- Modify: `apex_core/include/apex/core/spsc_queue.hpp`

- [ ] **Step 1: await 관련 include 및 멤버 추가**

includes에 추가:
```cpp
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <functional>
```

private 멤버 추가:
```cpp
    // Await backpressure
    std::atomic<bool> producer_waiting_{false};
    std::function<void(boost::system::error_code)> pending_handler_;
```

- [ ] **Step 2: public API 추가**

```cpp
    /// Awaitable enqueue. 큐 full이면 coroutine suspend → consumer drain 후 resume.
    boost::asio::awaitable<void> enqueue(const T& item);

    /// drain 후 호출 — 대기 중 producer resume 스케줄링.
    void notify_producer_if_waiting() noexcept;

    /// 셧다운 시 대기 중 producer 코루틴 정리.
    void cancel_waiting_producer() noexcept;
```

- [ ] **Step 3: enqueue() awaitable 구현**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
boost::asio::awaitable<void> SpscQueue<T>::enqueue(const T& item)
{
    if (try_enqueue(item))
        co_return;

    // Queue full → suspend via async_initiate
    co_await boost::asio::async_initiate<
        decltype(boost::asio::use_awaitable),
        void(boost::system::error_code)>(
        [this, &item](auto handler) {
            // Set waiting flag FIRST (release)
            producer_waiting_.store(true, std::memory_order_release);

            // Re-check: consumer may have drained between our full check and now
            auto consumed = consumed_.load(std::memory_order_acquire);
            if (head_ - consumed < capacity_)
            {
                // Space available — don't suspend
                producer_waiting_.store(false, std::memory_order_relaxed);
                auto ex = boost::asio::get_associated_executor(handler, producer_io_.get_executor());
                boost::asio::post(ex, [h = std::move(handler)]() mutable {
                    h(boost::system::error_code{});
                });
                return;
            }

            // Still full — store handler, consumer will call it during drain
            pending_handler_ = std::move(handler);
        },
        boost::asio::use_awaitable);

    // Resumed — enqueue must succeed now
    bool ok = try_enqueue(item);
    assert(ok && "SpscQueue::enqueue: try_enqueue after resume must succeed");
    (void)ok;
}
```

- [ ] **Step 4: notify_producer_if_waiting / cancel 구현**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
void SpscQueue<T>::notify_producer_if_waiting() noexcept
{
    if (!producer_waiting_.load(std::memory_order_acquire))
        return;

    producer_waiting_.store(false, std::memory_order_relaxed);

    if (pending_handler_)
    {
        auto handler = std::move(pending_handler_);
        boost::asio::post(producer_io_, [h = std::move(handler)]() mutable {
            h(boost::system::error_code{});
        });
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
void SpscQueue<T>::cancel_waiting_producer() noexcept
{
    if (!producer_waiting_.load(std::memory_order_acquire))
        return;

    producer_waiting_.store(false, std::memory_order_relaxed);

    if (pending_handler_)
    {
        auto handler = std::move(pending_handler_);
        boost::asio::post(producer_io_, [h = std::move(handler)]() mutable {
            h(boost::asio::error::operation_aborted);
        });
    }
}
```

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/spsc_queue.hpp
git commit -m "feat(core): BACKLOG-106 SpscQueue awaitable enqueue + cancel_waiting_producer"
```

---

## Task 4: SpscQueue Await 단위 테스트

**Files:**
- Modify: `apex_core/tests/unit/test_spsc_queue.cpp`

- [ ] **Step 1: await backpressure 테스트 추가**

```cpp
TEST_F(SpscQueueTest, AwaitEnqueue_SuspendsAndResumes)
{
    SpscQueue<int> q(2, io_ctx_);
    q.try_enqueue(1);
    q.try_enqueue(2);
    // Queue full

    bool resumed = false;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            co_await q.enqueue(3); // should suspend
            resumed = true;
        },
        boost::asio::detached);

    // Run io_context to start the coroutine — it should suspend
    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_FALSE(resumed);

    // Consumer drains
    q.try_dequeue();
    q.notify_producer_if_waiting();

    // Run io_context to resume the producer
    io_ctx_.restart();
    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(resumed);

    // Verify the item was enqueued
    q.try_dequeue(); // item 2
    auto v = q.try_dequeue(); // item 3
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 3);
}

TEST_F(SpscQueueTest, AwaitEnqueue_ImmediateIfNotFull)
{
    SpscQueue<int> q(4, io_ctx_);

    bool completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            co_await q.enqueue(42);
            completed = true;
        },
        boost::asio::detached);

    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(completed);
    EXPECT_EQ(*q.try_dequeue(), 42);
}

TEST_F(SpscQueueTest, AwaitEnqueue_ReCheckPreventsLostWakeup)
{
    // Re-check 프로토콜 검증: consumer가 producer_waiting 설정 전에 drain했을 때
    // producer가 suspend하지 않고 즉시 완료되는지 확인
    SpscQueue<int> q(2, io_ctx_);
    q.try_enqueue(1);
    q.try_enqueue(2);
    // Queue full

    // Consumer drains BEFORE producer sets waiting
    q.try_dequeue(); // space available now

    bool completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            co_await q.enqueue(3); // re-check should catch available space
            completed = true;
        },
        boost::asio::detached);

    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(completed); // Should complete without needing notify
}

TEST_F(SpscQueueTest, CancelWaitingProducer)
{
    SpscQueue<int> q(1, io_ctx_);
    q.try_enqueue(1); // full

    bool caught_error = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            try
            {
                co_await q.enqueue(2);
            }
            catch (const boost::system::system_error& e)
            {
                if (e.code() == boost::asio::error::operation_aborted)
                    caught_error = true;
            }
        },
        boost::asio::detached);

    io_ctx_.run_for(std::chrono::milliseconds{10});
    q.cancel_waiting_producer();
    io_ctx_.restart();
    io_ctx_.run_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(caught_error);
}
```

- [ ] **Step 2: 빌드 및 테스트**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug --target test_spsc_queue
apex_core/bin/debug/test_spsc_queue.exe
```

Expected: 전 테스트 PASS (기존 + 신규).

- [ ] **Step 3: 커밋**

```bash
git add apex_core/tests/unit/test_spsc_queue.cpp
git commit -m "test(core): BACKLOG-106 SpscQueue await backpressure 테스트"
```

---

## Task 5: SpscMesh 구현

**Files:**
- Create: `apex_core/include/apex/core/spsc_mesh.hpp`
- Create: `apex_core/src/spsc_mesh.cpp`
- Modify: `apex_core/CMakeLists.txt` (spsc_mesh.cpp 소스 추가)

**주의**: `spsc_mesh.hpp`는 `core_engine.hpp`를 포함하지 않는다 (순환 include 방지).
`io_context*` 벡터를 직접 받아서 CoreContext 의존성을 제거.

- [ ] **Step 1: spsc_mesh.hpp 작성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/spsc_queue.hpp>

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace apex::core
{

// Forward declarations — core_engine.hpp 포함 금지 (순환 include 방지)
struct CoreMessage;
class CrossCoreDispatcher;

/// N×(N-1) SPSC all-to-all mesh for inter-core communication.
/// Each core pair (src, dst) has a dedicated SpscQueue.
/// src == dst is disallowed (nullptr slot).
class SpscMesh
{
  public:
    /// @param num_cores Number of cores in the mesh
    /// @param queue_capacity Per-queue slot count (power-of-2)
    /// @param core_io_contexts 각 코어의 io_context 포인터 (producer io_context 바인딩용)
    SpscMesh(uint32_t num_cores, size_t queue_capacity,
             const std::vector<boost::asio::io_context*>& core_io_contexts);

    ~SpscMesh();

    SpscMesh(const SpscMesh&) = delete;
    SpscMesh& operator=(const SpscMesh&) = delete;

    /// Access the SPSC queue from src to dst. src != dst required (assert).
    [[nodiscard]] SpscQueue<CoreMessage>& queue(uint32_t src, uint32_t dst);

    /// Drain all incoming queues for dst_core. Dispatches messages via dispatcher.
    /// Also notifies waiting producers after drain.
    /// @return Total messages processed.
    size_t drain_all_for(uint32_t dst_core,
                         const CrossCoreDispatcher& dispatcher,
                         const std::function<void(uint32_t, const CoreMessage&)>& legacy_handler,
                         size_t batch_limit);

    /// Shutdown: cancel all waiting producers, drain remaining LegacyCrossCoreFn.
    void shutdown();

    [[nodiscard]] uint32_t core_count() const noexcept { return num_cores_; }

  private:
    uint32_t num_cores_;
    std::vector<std::unique_ptr<SpscQueue<CoreMessage>>> queues_; // [src * N + dst]
};

} // namespace apex::core
```

- [ ] **Step 2: spsc_mesh.cpp 작성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_mesh.hpp>
#include <apex/core/cross_core_dispatcher.hpp>
#include <apex/core/cross_core_op.hpp>

#include <spdlog/spdlog.h>

#include <cassert>
#include <functional>

namespace apex::core
{

SpscMesh::SpscMesh(uint32_t num_cores, size_t queue_capacity,
                   const std::vector<boost::asio::io_context*>& core_io_contexts)
    : num_cores_(num_cores)
{
    assert(core_io_contexts.size() == num_cores);
    queues_.resize(static_cast<size_t>(num_cores) * num_cores);
    for (uint32_t src = 0; src < num_cores; ++src)
    {
        for (uint32_t dst = 0; dst < num_cores; ++dst)
        {
            if (src == dst)
                continue;
            queues_[static_cast<size_t>(src) * num_cores + dst] =
                std::make_unique<SpscQueue<CoreMessage>>(
                    queue_capacity,
                    *core_io_contexts[src]); // producer(src)의 io_context
        }
    }
}

SpscMesh::~SpscMesh() = default;

SpscQueue<CoreMessage>& SpscMesh::queue(uint32_t src, uint32_t dst)
{
    assert(src < num_cores_ && dst < num_cores_ && "queue index out of range");
    assert(src != dst && "cannot send to self via SPSC mesh");
    auto& q = queues_[static_cast<size_t>(src) * num_cores_ + dst];
    assert(q && "queue must exist for src != dst");
    return *q;
}

size_t SpscMesh::drain_all_for(uint32_t dst_core,
                               const CrossCoreDispatcher& dispatcher,
                               const std::function<void(uint32_t, const CoreMessage&)>& legacy_handler,
                               size_t batch_limit)
{
    size_t total = 0;

    for (uint32_t src = 0; src < num_cores_; ++src)
    {
        if (src == dst_core)
            continue;
        if (total >= batch_limit)
            break;

        auto& q = queue(src, dst_core);
        while (total < batch_limit)
        {
            auto msg = q.try_dequeue();
            if (!msg)
                break;

            if (msg->op == CrossCoreOp::LegacyCrossCoreFn)
            {
                auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
                if (task)
                {
                    try
                    {
                        (*task)();
                    }
                    catch (const std::exception& e)
                    {
                        spdlog::error("Core {} cross-core task exception: {}", dst_core, e.what());
                    }
                    catch (...)
                    {
                        spdlog::error("Core {} cross-core task unknown exception", dst_core);
                    }
                    delete task;
                }
            }
            else if (dispatcher.has_handler(msg->op))
            {
                dispatcher.dispatch(dst_core, msg->source_core, msg->op,
                                    reinterpret_cast<void*>(msg->data));
            }
            else if (legacy_handler)
            {
                legacy_handler(dst_core, *msg);
            }
            ++total;
        }

        // Notify waiting producer for this queue
        q.notify_producer_if_waiting();
    }

    return total;
}

void SpscMesh::shutdown()
{
    for (uint32_t src = 0; src < num_cores_; ++src)
    {
        for (uint32_t dst = 0; dst < num_cores_; ++dst)
        {
            if (src == dst)
                continue;
            auto& q = queue(src, dst);

            // Cancel any waiting producers
            q.cancel_waiting_producer();

            // Drain remaining — clean up LegacyCrossCoreFn heap pointers
            while (auto msg = q.try_dequeue())
            {
                if (msg->op == CrossCoreOp::LegacyCrossCoreFn)
                {
                    auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
                    delete task;
                }
            }
        }
    }
}

} // namespace apex::core
```

- [ ] **Step 3: CMakeLists.txt에 소스 추가**

`apex_core/CMakeLists.txt`의 `add_library(apex_core STATIC ...)` 섹션에 `src/spsc_mesh.cpp` 추가 (`src/cross_core_dispatcher.cpp` 다음).

- [ ] **Step 4: 빌드 확인**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug --target apex_core
```

Expected: 컴파일 성공.

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/spsc_mesh.hpp apex_core/src/spsc_mesh.cpp apex_core/CMakeLists.txt
git commit -m "feat(core): BACKLOG-106 SpscMesh N×(N-1) all-to-all mesh 구현"
```

---

## Task 6: SpscMesh 단위 테스트

**Files:**
- Create: `apex_core/tests/unit/test_spsc_mesh.cpp`
- Modify: `apex_core/tests/unit/CMakeLists.txt`

- [ ] **Step 1: CMake 등록**

`test_spsc_queue` 바로 아래에:
```cmake
apex_add_unit_test(test_spsc_mesh test_spsc_mesh.cpp)
```

타임아웃 목록에 `test_spsc_mesh` 추가.

- [ ] **Step 2: 테스트 작성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_mesh.hpp>
#include <apex/core/cross_core_dispatcher.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace apex::core
{

class SpscMeshTest : public ::testing::Test
{
  protected:
    static constexpr uint32_t NUM_CORES = 4;
    static constexpr size_t QUEUE_CAPACITY = 64;

    void SetUp() override
    {
        for (uint32_t i = 0; i < NUM_CORES; ++i)
        {
            io_contexts_.push_back(std::make_unique<boost::asio::io_context>(1));
            io_ptrs_.push_back(io_contexts_.back().get());
        }
        mesh_ = std::make_unique<SpscMesh>(NUM_CORES, QUEUE_CAPACITY, io_ptrs_);
    }

    std::vector<std::unique_ptr<boost::asio::io_context>> io_contexts_;
    std::vector<boost::asio::io_context*> io_ptrs_;
    std::unique_ptr<SpscMesh> mesh_;
    CrossCoreDispatcher dispatcher_;
};

TEST_F(SpscMeshTest, QueueAccess_SrcDst)
{
    auto& q01 = mesh_->queue(0, 1);
    auto& q10 = mesh_->queue(1, 0);

    // Different queues for different directions
    EXPECT_NE(&q01, &q10);

    // Same queue for same src/dst
    EXPECT_EQ(&mesh_->queue(0, 1), &q01);
}

TEST_F(SpscMeshTest, QueueAccess_SelfAsserts)
{
    EXPECT_DEATH(mesh_->queue(0, 0), "cannot send to self");
}

TEST_F(SpscMeshTest, CoreCount)
{
    EXPECT_EQ(mesh_->core_count(), NUM_CORES);
}

TEST_F(SpscMeshTest, SingleCoreMode)
{
    boost::asio::io_context single_io{1};
    std::vector<boost::asio::io_context*> single_ptrs{&single_io};
    SpscMesh mesh(1, 64, single_ptrs);
    EXPECT_EQ(mesh.core_count(), 1u);
    // No queues to drain
    auto count = mesh.drain_all_for(0, dispatcher_, nullptr, 1024);
    EXPECT_EQ(count, 0u);
}

TEST_F(SpscMeshTest, DrainAllFor_ReceivesFromMultipleSources)
{
    CoreMessage msg0{.op = CrossCoreOp::Noop, .source_core = 0, .data = 100};
    CoreMessage msg2{.op = CrossCoreOp::Noop, .source_core = 2, .data = 200};

    mesh_->queue(0, 1).try_enqueue(msg0);
    mesh_->queue(2, 1).try_enqueue(msg2);

    std::vector<std::pair<uint32_t, uintptr_t>> received;
    auto handler = [&](uint32_t core_id, const CoreMessage& msg) {
        received.emplace_back(msg.source_core, msg.data);
    };

    auto count = mesh_->drain_all_for(1, dispatcher_, handler, 1024);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(received.size(), 2u);
}

TEST_F(SpscMeshTest, DrainAllFor_BatchLimit)
{
    // Fill queue 0→1 with 10 messages
    for (int i = 0; i < 10; ++i)
    {
        CoreMessage msg{.op = CrossCoreOp::Noop, .source_core = 0, .data = static_cast<uintptr_t>(i)};
        mesh_->queue(0, 1).try_enqueue(msg);
    }

    auto count = mesh_->drain_all_for(1, dispatcher_, nullptr, 5);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(mesh_->queue(0, 1).size_approx(), 5u); // 5 remaining
}

TEST_F(SpscMeshTest, Shutdown_CleansLegacyClosures)
{
    std::atomic<bool> deleted{false};
    auto* task = new std::function<void()>([&deleted] { deleted = true; });

    CoreMessage msg{.op = CrossCoreOp::LegacyCrossCoreFn,
                    .source_core = 0,
                    .data = reinterpret_cast<uintptr_t>(task)};
    mesh_->queue(0, 1).try_enqueue(msg);

    mesh_->shutdown();
    // task should be deleted (not executed) during shutdown drain
    // No leak — ASAN will catch if not deleted
}

} // namespace apex::core
```

- [ ] **Step 3: 빌드 및 테스트**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug --target test_spsc_mesh
apex_core/bin/debug/test_spsc_mesh.exe
```

Expected: 전 테스트 PASS.

- [ ] **Step 4: 커밋**

```bash
git add apex_core/tests/unit/test_spsc_mesh.cpp apex_core/tests/unit/CMakeLists.txt
git commit -m "test(core): BACKLOG-106 SpscMesh 단위 테스트"
```

---

## Task 7: CoreEngine 통합 — MPSC→SPSC 교체

가장 큰 변경. CoreEngine의 MPSC inbox를 SpscMesh로 교체하고 post_to()를 awaitable로 전환.

**Files:**
- Modify: `apex_core/include/apex/core/core_engine.hpp`
- Modify: `apex_core/src/core_engine.cpp`

- [ ] **Step 1: core_engine.hpp 수정**

주요 변경:
1. `#include <apex/core/spsc_mesh.hpp>` 추가 (mpsc_queue.hpp는 유틸리티로 보존하되 CoreContext에서 사용 중지)
2. `CoreEngineConfig`: `mpsc_queue_capacity{65536}` → `spsc_queue_capacity{1024}`
3. `CoreContext`: `inbox` 멤버 제거, 생성자 시그니처 변경 `CoreContext(uint32_t id)` (queue_capacity 파라미터 제거)
4. `CoreEngine::post_to()`: `Result<void>` → `boost::asio::awaitable<void>`, source_core는 `current_core_id()`로 취득
5. `CoreEngine::broadcast()`: `post_to()` 대신 `asio::post(io_context, callback)` 사용으로 전환 (자기 코어 포함 가능, SPSC mesh 미사용)
6. Private: `mesh_` (`std::unique_ptr<SpscMesh>`) 추가
7. `schedule_drain()`: inbox->empty() 참조 제거 → mesh 기반 empty 체크로 전환
8. `drain_inbox()`: `mesh_->drain_all_for()` 위임
9. `drain_remaining()`: `mesh_->shutdown()` 위임

**CoreContext 생성자 변경 (BLOCKING)**:
```cpp
// Before (core_engine.hpp:56)
CoreContext(uint32_t id, size_t queue_capacity);

// After
explicit CoreContext(uint32_t id);
```

```cpp
// Before (core_engine.cpp:23-26)
CoreContext::CoreContext(uint32_t id, size_t queue_capacity)
    : core_id(id)
    , inbox(std::make_unique<MpscQueue<CoreMessage>>(queue_capacity))
{}

// After
CoreContext::CoreContext(uint32_t id) : core_id(id) {}
```

**broadcast() 수정 (BLOCKING)**:
```cpp
// Before: post_to()는 awaitable이 되므로 동기 broadcast에서 호출 불가.
// 또한 post_to(self)가 assert 실패.
// After: asio::post 직접 사용 (비코어 스레드에서도 안전).
void CoreEngine::broadcast(CoreMessage msg)
{
    // LegacyCrossCoreFn은 raw pointer를 소유 — N개 코어에 복사하면 double-delete.
    // broadcast는 Custom op + non-owned data 전용.
    assert(msg.op != CrossCoreOp::LegacyCrossCoreFn &&
           "broadcast() cannot be used with LegacyCrossCoreFn (raw pointer ownership)");

    for (uint32_t i = 0; i < cores_.size(); ++i)
    {
        boost::asio::post(cores_[i]->io_ctx, [this, i, msg] {
            if (cross_core_dispatcher_.has_handler(msg.op))
            {
                cross_core_dispatcher_.dispatch(i, msg.source_core, msg.op,
                                                reinterpret_cast<void*>(msg.data));
            }
            else if (message_handler_)
            {
                message_handler_(i, msg);
            }
        });
    }
}
```

**schedule_drain() 수정 (BLOCKING)**:
```cpp
// Before: inbox->empty() 참조
// After: mesh 기반 empty 체크
void CoreEngine::schedule_drain(uint32_t target_core)
{
    if (!drain_pending_[target_core].exchange(true, std::memory_order_acq_rel))
    {
        boost::asio::post(cores_[target_core]->io_ctx, [this, target_core] {
            drain_inbox(target_core);
            drain_pending_[target_core].store(false, std::memory_order_release);
            // Re-check: 아직 메시지가 남아있을 수 있음
            // mesh의 모든 수신 큐를 확인
            if (mesh_)
            {
                for (uint32_t src = 0; src < core_count(); ++src)
                {
                    if (src == target_core) continue;
                    if (!mesh_->queue(src, target_core).empty())
                    {
                        schedule_drain(target_core);
                        return;
                    }
                }
            }
        });
    }
}
```

- [ ] **Step 2: core_engine.cpp 수정**

핵심 변경:

**생성자**: MpscQueue 대신 SpscMesh 생성
```cpp
CoreEngine::CoreEngine(CoreEngineConfig config) : config_(config)
{
    // ... num_cores auto-detect (기존 유지) ...

    // drain_pending_ 먼저 초기화 (기존 순서 유지)
    drain_pending_ = std::make_unique<std::atomic<bool>[]>(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i)
        drain_pending_[i].store(false, std::memory_order_relaxed);

    cores_.reserve(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i)
        cores_.push_back(std::make_unique<CoreContext>(i));

    // SpscMesh 생성 (N≥2일 때만)
    // cores_ → io_context* 벡터 변환 (SpscMesh는 core_engine.hpp에 의존하지 않음)
    if (config_.num_cores > 1)
    {
        std::vector<boost::asio::io_context*> io_ptrs;
        io_ptrs.reserve(config_.num_cores);
        for (auto& ctx : cores_)
            io_ptrs.push_back(&ctx->io_ctx);

        mesh_ = std::make_unique<SpscMesh>(
            config_.num_cores, config_.spsc_queue_capacity, io_ptrs);
    }
}
```

**CoreContext 생성자**: inbox 파라미터 제거
```cpp
CoreContext::CoreContext(uint32_t id) : core_id(id) {}
```

**post_to()**: awaitable 전환
```cpp
boost::asio::awaitable<void> CoreEngine::post_to(uint32_t target_core, CoreMessage msg)
{
    assert(target_core < cores_.size() && "target_core out of range");
    assert(target_core != current_core_id() && "cannot post to self via SPSC mesh");

    auto source_core = current_core_id();
    assert(source_core < cores_.size() && "post_to must be called from a core thread");

    cores_[target_core]->metrics.post_total.fetch_add(1, std::memory_order_relaxed);

    co_await mesh_->queue(source_core, target_core).enqueue(msg);
    schedule_drain(target_core);
}
```

**drain_inbox()**: SpscMesh 기반으로 전환
```cpp
void CoreEngine::drain_inbox(uint32_t core_id)
{
    if (!mesh_) return;
    mesh_->drain_all_for(core_id, cross_core_dispatcher_,
                         message_handler_ ? [this](uint32_t cid, const CoreMessage& msg) {
                             message_handler_(cid, msg);
                         } : std::function<void(uint32_t, const CoreMessage&)>{},
                         config_.drain_batch_limit);
}
```

**drain_remaining()**: mesh shutdown 위임
```cpp
void CoreEngine::drain_remaining()
{
    assert(!running_.load() && threads_.empty());
    if (mesh_) mesh_->shutdown();
}
```

**broadcast()**: 단순 루프 유지 (동기 API, 비코어 스레드용)
— 이 함수는 asio::post() 직접 사용으로 전환하거나, 사용처가 없으면 제거 검토.

- [ ] **Step 3: 빌드 확인**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug --target apex_core
```

컴파일 에러 발생 예상 — cross_core_call.hpp, 서비스 코드에서 `Result<void>` 시그니처 불일치. Task 8에서 해결.

- [ ] **Step 4: 커밋 (WIP)**

```bash
git add apex_core/include/apex/core/core_engine.hpp apex_core/src/core_engine.cpp
git commit -m "wip(core): BACKLOG-106 CoreEngine MPSC→SPSC mesh 교체 (API 전환 진행 중)"
```

---

## Task 8: Cross-Core API 전환

**Files:**
- Modify: `apex_core/include/apex/core/cross_core_call.hpp`

- [ ] **Step 1: cross_core_post_msg() awaitable 전환**

```cpp
/// Zero-allocation fire-and-forget message passing via CrossCoreOp.
/// 코어→코어 전용. 비코어 스레드에서는 asio::post(engine.io_context(core_id)) 사용.
inline boost::asio::awaitable<void> cross_core_post_msg(
    CoreEngine& engine, uint32_t source_core, uint32_t target_core,
    CrossCoreOp op, void* data = nullptr)
{
    assert(source_core < engine.core_count() && "invalid source_core");
    CoreMessage msg{.op = op, .source_core = source_core,
                    .data = reinterpret_cast<uintptr_t>(data)};
    co_await engine.post_to(target_core, msg);
}
```

- [ ] **Step 2: cross_core_post() awaitable 전환**

```cpp
/// Fire-and-forget closure execution on target core (코어→코어 전용).
template <typename F>
boost::asio::awaitable<void> cross_core_post(CoreEngine& engine, uint32_t target_core, F&& func)
{
    auto* task = new std::function<void()>(std::forward<F>(func));
    CoreMessage msg;
    msg.op = CrossCoreOp::LegacyCrossCoreFn;
    msg.data = reinterpret_cast<uintptr_t>(task);
    try
    {
        co_await engine.post_to(target_core, msg);
    }
    catch (...)
    {
        delete task;
        throw;
    }
}
```

- [ ] **Step 3: broadcast_cross_core() awaitable 전환**

```cpp
inline boost::asio::awaitable<void> broadcast_cross_core(
    CoreEngine& engine, uint32_t source_core, CrossCoreOp op, SharedPayload* payload)
{
    assert(payload != nullptr);
    if (engine.core_count() <= 1) { delete payload; co_return; }
    assert(payload->refcount() > 0);

    for (uint32_t i = 0; i < engine.core_count(); ++i)
    {
        if (i == source_core) continue;
        CoreMessage msg{.op = op, .source_core = source_core,
                        .data = reinterpret_cast<uintptr_t>(payload)};
        co_await engine.post_to(i, msg);
    }
}
```

- [ ] **Step 4: cross_core_call() 내부 post_to() co_await 전환**

`cross_core_call` 두 오버로드에서 `engine.post_to()` 호출 부분을 `co_await engine.post_to()`로 변경. 에러 반환 로직(`if (!post_result)`)을 제거 — awaitable이므로 backpressure가 자동 처리됨. `ErrorCode::CrossCoreQueueFull` 경로 제거.

- [ ] **Step 5: Server::cross_core_post() awaitable 전환**

`apex_core/include/apex/core/server.hpp:290`:
```cpp
// Before
template <typename F> Result<void> cross_core_post(uint32_t target_core, F&& func)
{
    return apex::core::cross_core_post(*core_engine_, target_core, std::forward<F>(func));
}

// After
template <typename F> boost::asio::awaitable<void> cross_core_post(uint32_t target_core, F&& func)
{
    co_await apex::core::cross_core_post(*core_engine_, target_core, std::forward<F>(func));
}
```

- [ ] **Step 6: 커밋**

```bash
git add apex_core/include/apex/core/cross_core_call.hpp apex_core/include/apex/core/server.hpp
git commit -m "feat(core): BACKLOG-106 cross-core API + Server wrapper awaitable 전환"
```

---

## Task 9: BroadcastFanout 마이그레이션

**Files:**
- Modify: `apex_services/gateway/src/broadcast_fanout.cpp`
- Modify: `apex_services/gateway/include/apex/gateway/broadcast_fanout.hpp` (필요 시)

- [ ] **Step 1: cross_core_post → asio::post 전환**

`broadcast_fanout.cpp`에서 `cross_core_post(engine_, core_id, ...)` 호출 2건을 `boost::asio::post(engine_.io_context(core_id), ...)` 로 교체.

Line 45 (global broadcast):
```cpp
// Before: (void)apex::core::cross_core_post(engine_, core_id, [...]{...});
// After:
boost::asio::post(engine_.io_context(core_id), [mgr, shared_data = data]() {
    mgr->for_each([&shared_data](apex::core::SessionPtr session) {
        if (session && session->is_open())
        {
            (void)session->enqueue_write_raw(*shared_data);
        }
    });
});
```

Line 67 (room broadcast): 동일 패턴.

`#include <apex/core/cross_core_call.hpp>` → `#include <boost/asio/post.hpp>` + `#include <apex/core/core_engine.hpp>` (이미 포함 가능)

- [ ] **Step 2: 빌드 확인**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug
```

- [ ] **Step 3: 커밋**

```bash
git add apex_services/gateway/src/broadcast_fanout.cpp
git commit -m "refactor(gateway): BACKLOG-106 BroadcastFanout cross_core_post → asio::post 전환"
```

---

## Task 10: 기존 테스트 마이그레이션

**Files:**
- Modify: `apex_core/tests/unit/test_cross_core_call.cpp`
- Modify: `apex_core/tests/unit/test_core_engine.cpp`

- [ ] **Step 1: test_cross_core_call.cpp 전환**

현재 동기 호출을 코루틴 컨텍스트에서 호출하도록 전환. 3가지 패턴:

**패턴 A: 코어 스레드에서 호출 (대부분)**
```cpp
// Before (동기):
auto result = cross_core_post_msg(engine, 0, 1, op, &payload);
EXPECT_TRUE(result.has_value());

// After (코루틴 — engine이 running 상태일 때):
// core 0의 io_context에서 코루틴 스폰
std::atomic<bool> done{false};
boost::asio::co_spawn(engine.io_context(0), [&]() -> boost::asio::awaitable<void> {
    co_await cross_core_post_msg(engine, 0, 1, op, &payload);
    done.store(true, std::memory_order_release);
}, boost::asio::detached);
ASSERT_TRUE(wait_for([&]{ return done.load(); }));
```

**패턴 B: 비코어 스레드에서 직접 호출하던 테스트 → 재설계**

`PostMsgToInvalidCore` (line 318): `post_to()` assert로 전환되었으므로 DEATH 테스트로 변경하거나, 범위 체크를 별도 검증.

`PostMsgQueueFull` (lines 332-335): backpressure로 전환. 큐 full → suspend 동작 검증:
```cpp
// QueueFull 테스트 → backpressure 테스트로 재설계
// SpscQueue 테스트(Task 4)에서 이미 커버하므로, 여기서는 통합 수준 확인
TEST_F(CrossCoreCallTest, PostMsg_BackpressureOnFullQueue)
{
    // 작은 큐 용량으로 CoreEngine 구성
    // 큐 채운 후 co_await post_to → suspend → drain → resume 확인
}
```

**패턴 C: Server wrapper 테스트**

`server_->cross_core_post()` (line 96): 반환 타입이 `awaitable<void>`로 변경.
```cpp
// Before:
auto posted = server_->cross_core_post(1, [&value] { value.store(99); });
EXPECT_TRUE(posted.has_value());

// After: co_spawn 내에서 호출
boost::asio::co_spawn(server_->io_context(0), [&]() -> boost::asio::awaitable<void> {
    co_await server_->cross_core_post(1, [&value] { value.store(99); });
}, boost::asio::detached);
```

**패턴 D: broadcast with queue full → backpressure 테스트로 재설계**

`BroadcastWithQueueFull` (line 380): awaitable broadcast는 suspend하므로 "partial failure" 시나리오가 제거됨.
SharedPayload refcount 관리 테스트로 재설계:
```cpp
TEST_F(CrossCoreCallTest, Broadcast_AllCoresReceive)
{
    // broadcast_cross_core를 co_spawn 내에서 호출
    // 모든 타겟 코어가 메시지를 수신하는지 확인 (유실 없음 보장)
}
```

13개 call site 전부 전환. 각 테스트 케이스를 개별 확인하며 진행.

- [ ] **Step 2: test_core_engine.cpp 확인**

`CoreContext::inbox` 제거 영향. `post_to()` 시그니처 변경 영향. 기존 테스트가 `engine.post_to()` 동기 호출하는 곳을 코루틴으로 전환.

**broadcast() 테스트 (line 106)**: `engine.broadcast(msg)` — broadcast가 `asio::post` 직접 사용으로 전환되었으므로 테스트 자체는 호환. 단, broadcast가 자기 코어에도 전달되므로 테스트의 `count.load() >= num_cores` 기대값 확인.

- [ ] **Step 3: 전체 테스트 빌드 및 실행**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug
```

전체 단위 테스트 실행:
```bash
cd apex_core && ctest --test-dir build/debug --output-on-failure -j1
```

Expected: 전 테스트 PASS.

- [ ] **Step 4: 커밋**

```bash
git add apex_core/tests/unit/test_cross_core_call.cpp apex_core/tests/unit/test_core_engine.cpp
git commit -m "test(core): BACKLOG-106 기존 크로스코어 테스트 awaitable 전환"
```

---

## Task 11: 벤치마크

**Files:**
- Create: `apex_core/benchmarks/micro/bench_spsc_queue.cpp`
- Create: `apex_core/benchmarks/integration/bench_spsc_mesh_contention.cpp`
- Modify: `apex_core/benchmarks/CMakeLists.txt`
- Modify: `apex_core/benchmarks/integration/bench_cross_core_latency.cpp`
- Modify: `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp`

- [ ] **Step 1: CMake 등록**

`apex_core/benchmarks/CMakeLists.txt`에 추가:

```cmake
# SPSC
apex_add_benchmark(bench_spsc_queue             micro/bench_spsc_queue.cpp)
apex_add_benchmark(bench_spsc_mesh_contention   integration/bench_spsc_mesh_contention.cpp)
```

- [ ] **Step 2: bench_spsc_queue.cpp 작성**

MpscQueue 벤치마크(`bench_mpsc_queue.cpp`)와 동일 구조. 3가지 벤치마크:
- `BM_SpscQueue_Throughput`: 1P:1C, N개 메시지 enqueue/dequeue
- `BM_SpscQueue_Latency`: enqueue→dequeue 단방향 지연
- `BM_SpscQueue_Backpressure`: full 큐 await resume 지연

- [ ] **Step 3: bench_spsc_mesh_contention.cpp 작성**

N코어 동시 전송 worst-case. 코어 수 4/8/16/32/48 파라미터. MPSC vs SPSC 비교 불필요 — SPSC mesh 단독 throughput/latency 측정. (MPSC 비교는 기존 bench_mpsc_queue로 별도 수행.)

- [ ] **Step 4: 기존 벤치마크 SPSC 시나리오 추가**

`bench_cross_core_latency.cpp`, `bench_cross_core_message_passing.cpp`에 SPSC mesh 기반 시나리오 추가. 기존 MPSC 시나리오는 제거 (CoreEngine이 SPSC로 전환되었으므로).

- [ ] **Step 5: 빌드 확인**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug
```

- [ ] **Step 6: 커밋**

```bash
git add apex_core/benchmarks/
git commit -m "bench(core): BACKLOG-106 SpscQueue + mesh contention 벤치마크"
```

---

## Task 12: clang-format + 최종 빌드 + 전체 테스트

**Files:** 전체 변경 파일

- [ ] **Step 1: clang-format**

```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] **Step 2: 전체 빌드**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug
```

Expected: 0 warnings, 0 errors.

- [ ] **Step 3: 전체 테스트**

```bash
cd apex_core && ctest --test-dir build/debug --output-on-failure -j1
```

Expected: 전 테스트 PASS.

- [ ] **Step 4: 포맷 diff 커밋 (있으면)**

```bash
git add -A
git commit -m "style(core): BACKLOG-106 clang-format 적용"
```

---

## Task 13: 문서 갱신

머지 전 필수 갱신 대상.

**Files:**
- Modify: `docs/Apex_Pipeline.md` (§4 아키텍처, §9 API 테이블)
- Modify: `docs/apex_core/apex_core_guide.md` (CoreEngine 아키텍처, cross-core API)
- Modify: `docs/BACKLOG.md` (#106 완료 → BACKLOG_HISTORY.md 이전)
- Modify: `CLAUDE.md` (로드맵 버전)
- Modify: `README.md` (필요 시)

- [ ] **Step 1: Apex_Pipeline.md 갱신**

§4 기법 테이블: "Lock-free MPSC Queue" → "SPSC All-to-All Mesh" 전환 반영
§9 API 테이블: `cross_core_post_msg` 등 반환 타입 `awaitable<void>` 갱신, 비코어 스레드 경로 설명 추가

- [ ] **Step 2: apex_core_guide.md 갱신**

CoreEngine 아키텍처 다이어그램에서 MPSC → SPSC mesh 반영. Cross-core message path 갱신.

- [ ] **Step 3: BACKLOG 정리**

#106 완료 → `docs/BACKLOG_HISTORY.md`에 이전.

- [ ] **Step 4: 커밋**

```bash
git add docs/ CLAUDE.md README.md
git commit -m "docs(core): BACKLOG-106 SPSC mesh 전환 문서 갱신"
```
