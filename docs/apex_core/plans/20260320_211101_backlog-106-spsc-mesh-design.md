# BACKLOG-106: CrossCoreDispatcher MPSC→SPSC All-to-All Mesh 리팩토링 설계

- **버전**: v0.5.10.0 (예정)
- **스코프**: apex_core
- **등급**: MAJOR / perf
- **연관**: BACKLOG-20 (BumpAllocator/ArenaAllocator 벤치마크)

## 1. 동기

### 현재 구조 (MPSC N:1)

각 코어가 하나의 `MpscQueue<CoreMessage>` inbox를 소유. 모든 코어가 동일한 `head_` atomic에 CAS 경합.

```
Core 0 ──┐
Core 1 ──┼──→ [MpscQueue] ──→ Core 2 (drain)
Core 3 ──┘
```

- `head_`: `atomic<size_t>` + CAS — N-1개 producer가 경합
- 코어 수 증가 시 CAS contention + cache line bouncing 선형 증가
- 48코어: 47개 producer가 단일 cache line에 경합

### 목표 구조 (SPSC N×(N-1) Mesh)

각 코어 쌍에 전용 SPSC 큐를 할당. CAS 완전 제거.

```
Core 0 → Core 1: [SpscQueue]
Core 0 → Core 2: [SpscQueue]
Core 1 → Core 0: [SpscQueue]
Core 1 → Core 2: [SpscQueue]
Core 2 → Core 0: [SpscQueue]
Core 2 → Core 1: [SpscQueue]
```

### 전환 근거

1. **스케일업 대비**: 서버 머신 최소 48코어 이상 상정. N이 클수록 MPSC CAS contention 악화
2. **구조적 정합성**: source_core를 이미 알고 있으므로 1:1 관계가 자연스러움. shared-nothing 원칙에 강력히 부합
3. **성능**: CAS 제거 + cache line bouncing 제거 → wait-free enqueue/dequeue

## 2. 설계 결정 요약

| 결정 항목 | 선택 | 근거 |
|-----------|------|------|
| 토폴로지 | SPSC all-to-all mesh (N×(N-1)) | shared-nothing, CAS 제거 |
| 슬롯 용량 | 1,024 per queue | Seastar 128 대비 8x 마진. event-driven drain 특성 감안. 256코어 시 ~1GB |
| Backpressure | await (코루틴 suspend) | 메시지 유실 방지. Seastar submit_to()와 동일 모델 |
| Drain 전략 | 통합 트리거 + atomic coalescing | sweep 비용 무시 가능, coalescing으로 burst 효율 극대화 |
| 큐 구현 | Custom SpscQueue\<T\> | MpscQueue와 동일 패턴, 런타임 용량 설정, cache line 완전 제어 |
| API | 코어→코어: awaitable 전환, 비코어→코어: asio::post() 직접 사용 | Seastar alien::submit_to() 패턴 |
| MpscQueue | 유틸리티로 보존 | 벤치마크 비교 + 범용 MPSC 용도 |

### 업계 사례 비교

| 프레임워크 | drain 방식 | SPSC 용량 | 비고 |
|-----------|-----------|----------|------|
| Seastar (ScyllaDB/Redpanda) | busy-poll (매 reactor loop) | 128 | boost::lockfree::spsc_queue |
| DPDK | busy-poll | 수백~수천 | rte_ring SP/SC 모드 |
| LMAX Disruptor | busy-spin / yield | 1,024~8,192 | L3 캐시 적합 권장 |
| **Apex (본 설계)** | **event-driven (asio::post)** | **1,024** | Custom SpscQueue + await |

## 3. 스레드 모델: 코어 스레드 vs 비코어 스레드

### 두 가지 메시지 경로

SPSC mesh는 **코어→코어 전용**이다. 비코어 스레드(어댑터 콜백 등)는 별도 경로를 사용한다.

```
[코어→코어] Core A ──→ SpscQueue(A,B) ──→ Core B (drain)
                     awaitable, backpressure 지원

[비코어→코어] Redis/Kafka Thread ──→ asio::post(Core B io_ctx, callback)
                                   기존 Asio 메커니즘, 큐 불필요
```

| 경로 | 사용 조건 | 메커니즘 | Backpressure |
|------|----------|----------|-------------|
| **코어→코어** | 양쪽 모두 유효한 core_id 보유 | SPSC mesh + awaitable | co_await (suspend/resume) |
| **비코어→코어** | 발신자가 코어 스레드가 아님 | `asio::post(io_ctx, callback)` | Asio 내부 큐 (unbounded) |

### 기존 비코어 스레드 caller 영향

| Caller | 현재 방식 | 전환 후 |
|--------|----------|---------|
| `BroadcastFanout::fanout()` | `cross_core_post(engine, core_id, ...)` (Redis 스레드) | `asio::post(engine.io_context(core_id), callback)` 직접 사용 |
| `ResponseDispatcher::on_response()` | `asio::post(io_ctx, callback)` (Kafka 스레드) | 변경 없음 (이미 올바른 패턴) |
| 테스트 코드 | `cross_core_post_msg(...)` | 테스트 내 `asio::post()` 전환 또는 코루틴 테스트 헬퍼 사용 |

`BroadcastFanout`의 `cross_core_post()` → `asio::post()` 전환은 Seastar의 `alien::submit_to()` 패턴과 동일한 설계 원칙이다: **비코어 스레드는 SPSC mesh에 접근하지 않는다.**

## 4. SpscQueue\<T\> 설계

### 인터페이스

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
class SpscQueue
{
public:
    /// @param capacity 큐 용량 (power-of-2로 반올림)
    /// @param producer_io producer 코어의 io_context (resume 전달용)
    explicit SpscQueue(size_t capacity, boost::asio::io_context& producer_io);

    // === Producer API (단일 스레드) ===
    bool try_enqueue(const T& item) noexcept;                       // non-blocking
    boost::asio::awaitable<void> enqueue(const T& item);            // awaitable

    // === Consumer API (단일 스레드) ===
    std::optional<T> try_dequeue() noexcept;
    size_t drain(std::span<T> batch) noexcept;

    /// drain 후 호출 — 대기 중인 producer가 있으면 resume 스케줄링
    void notify_producer_if_waiting() noexcept;

    /// 셧다운 시 호출 — 대기 중인 producer 코루틴 정리
    void cancel_waiting_producer() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] size_t size() const noexcept;

private:
    struct Slot {
        T data;                                     // CoreMessage: 16B
    };

    alignas(64) size_t head_{0};                    // producer only — plain, no CAS
    alignas(64) size_t tail_{0};                    // consumer only — plain
    alignas(64) std::atomic<size_t> published_{0};  // producer → consumer (release/acquire)
    alignas(64) std::atomic<size_t> consumed_{0};   // consumer → producer (release/acquire)

    size_t capacity_;
    size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    // Await 지원
    boost::asio::io_context& producer_io_;          // producer 코어의 io_context
    std::atomic<bool> producer_waiting_{false};      // consumer가 읽음
};
```

### 메모리 레이아웃

- 각 핫 필드를 `alignas(64)`로 캐시 라인 격리 → false sharing 방지
- `head_`/`tail_`은 plain `size_t` (각각 단일 스레드 소유, atomic 불필요)
- `published_`/`consumed_`만 atomic — acquire-release 시맨틱
- `Slot`은 `T data`만 보유 (MpscQueue의 `atomic<bool> ready` 불필요 — SPSC는 published/consumed 카운터로 충분)
- `capacity_`는 생성자에서 power-of-2로 반올림

### Slot 크기와 메모리

`Slot`에 ready flag가 없으므로 `sizeof(Slot) == sizeof(T)`. CoreMessage 기준:

- `sizeof(CoreMessage)` = 16B (static_assert 적용)
- `sizeof(Slot)` = 16B
- 큐당 메모리: 1,024 × 16B = **16KB**

### MpscQueue와의 차이

| | MpscQueue | SpscQueue |
|---|---|---|
| head_ | `atomic<size_t>` + CAS | plain `size_t` |
| per-slot 동기화 | `atomic<bool> ready` (24B/slot) | 없음 (16B/slot) |
| 전역 동기화 | CAS | published/consumed 카운터 |
| Producer | multi-thread | single-thread |
| Backpressure | `ErrorCode::CrossCoreQueueFull` 반환 | `co_await` suspend/resume |

### Await 메커니즘 — Lost Wakeup 방지 프로토콜

```
Producer (Core A)                        Consumer (Core B)
─────────────────                        ─────────────────
enqueue(msg):
  1. 큐 full 확인 (published_ - consumed_ >= capacity_)
  2. producer_waiting_.store(true, release)
  3. ── Re-check ──
     consumed_ 재확인 (acquire)
     공간 생겼으면:
       producer_waiting_.store(false, relaxed)
       → enqueue 직접 수행, co_return
  4. 여전히 full → async_initiate로 SUSPEND
                                         drain():
                                           5. consumed_.store(new_tail, release)
                                           6. producer_waiting_.load(acquire)
                                              → true면:
                                                asio::post(producer_io_, resume)
  ← RESUME (producer 코어의 io_context에서)
  7. enqueue 재시도 (성공)
```

**Re-check (3단계)가 핵심**: `producer_waiting_ = true` 설정 후 `consumed_`를 다시 확인하여, consumer가 이미 drain했지만 flag를 보기 전인 시나리오를 방지한다.

### Asio 통합 — async_initiate 패턴

raw `std::coroutine_handle<>` 대신 Asio의 `async_initiate` + `use_awaitable` 사용:

```cpp
template <typename T>
boost::asio::awaitable<void> SpscQueue<T>::enqueue(const T& item)
{
    if (try_enqueue(item))
        co_return;

    // full → async operation으로 suspend
    co_await boost::asio::async_initiate<
        decltype(boost::asio::use_awaitable),
        void(boost::system::error_code)>(
        [this, &item](auto handler) {
            producer_waiting_.store(true, std::memory_order_release);

            // Re-check: consumer가 이미 drain했을 수 있음
            auto consumed = consumed_.load(std::memory_order_acquire);
            if (published_.load(std::memory_order_relaxed) - consumed < capacity_) {
                producer_waiting_.store(false, std::memory_order_relaxed);
                auto ex = boost::asio::get_associated_executor(handler);
                boost::asio::post(ex, [h = std::move(handler)]() mutable {
                    h(boost::system::error_code{});
                });
                return;
            }

            // 여전히 full → handler 저장, consumer drain 시 호출됨
            pending_handler_ = std::move(handler);
        },
        boost::asio::use_awaitable);

    // resume 후 enqueue
    bool ok = try_enqueue(item);
    assert(ok && "enqueue after resume must succeed");
}
```

## 5. SpscMesh 설계

### 인터페이스

```cpp
class SpscMesh
{
public:
    SpscMesh(uint32_t num_cores, size_t queue_capacity,
             std::span<CoreContext> cores);

    SpscQueue<CoreMessage>& queue(uint32_t src, uint32_t dst);

    size_t drain_all_for(uint32_t dst_core,
                         const CrossCoreDispatcher& dispatcher);

    /// 셧다운: 모든 잔여 메시지 drain + 대기 중 producer 정리
    void shutdown();

    [[nodiscard]] uint32_t core_count() const noexcept;

private:
    uint32_t num_cores_;
    std::vector<std::unique_ptr<SpscQueue<CoreMessage>>> queues_;

    struct alignas(64) DrainFlag {
        std::atomic<bool> pending{false};
    };
    std::vector<DrainFlag> drain_flags_;
};
```

### 메모리 구조

- Flat vector + 인덱싱: `queues_[src * num_cores + dst]`
- `src == dst` 슬롯은 nullptr (자기 자신에게 보내기 금지, assert)
- `DrainFlag` per-core: `alignas(64)`로 cache line 격리
- **단일 코어 모드 (N=1)**: `SpscMesh` 생성 시 큐 0개 (N*(N-1)=0). `drain_all_for()` 즉시 반환. `post_to()` 호출 시 assert 실패 (자기 자신에게 보낼 수 없음)

### SpscQueue 생성 시 io_context 바인딩

```cpp
SpscMesh::SpscMesh(uint32_t num_cores, size_t queue_capacity,
                   std::span<CoreContext> cores)
{
    queues_.resize(num_cores * num_cores);
    for (uint32_t src = 0; src < num_cores; ++src)
        for (uint32_t dst = 0; dst < num_cores; ++dst) {
            if (src == dst) continue;
            queues_[src * num_cores + dst] =
                std::make_unique<SpscQueue<CoreMessage>>(
                    queue_capacity,
                    cores[src].io_ctx);   // producer(src)의 io_context
        }
}
```

### Drain 동작

```
post_to(src, dst, msg):
  1. co_await mesh_.queue(src, dst).enqueue(msg)
  2. if (!drain_flags_[dst].pending.exchange(true, acquire))
       asio::post(cores_[dst].io_ctx, [this, dst]{ drain_all_for(dst); })

drain_all_for(dst):
  3. drain_flags_[dst].pending.store(false, release)
  4. for src in [0..N-1], src != dst:
       while (auto msg = mesh_.queue(src, dst).try_dequeue())
           dispatcher.dispatch(dst, msg.source_core, msg.op, data)
  5. 각 큐에 대해 notify_producer_if_waiting() 호출
```

**drain_flags_ ordering**: `store(false, release)` 사용. ARM 등 weak-memory 아키텍처에서 drain 완료 전에 flag가 cleared로 보이는 reorder 방지. 현행 `core_engine.cpp`의 `drain_pending_` 패턴과 동일.

### 셧다운 시퀀스

```
CoreEngine::shutdown():
  1. 각 코어 이벤트 루프 종료 신호
  2. mesh_->shutdown():
     a. 모든 큐의 cancel_waiting_producer() — 대기 중 코루틴 정리
     b. 모든 큐의 잔여 메시지 drain (LegacyCrossCoreFn은 delete 처리)
  3. 코어 스레드 join
```

### 메모리 사용량

| 코어 수 | 큐 수 | 총 메모리 (16B/slot × 1,024) |
|---------|-------|------------------------------|
| 8 | 56 | ~896KB |
| 48 | 2,256 | ~35MB |
| 128 | 16,256 | ~254MB |
| 256 | 65,280 | ~1.0GB |

## 6. CoreEngine 통합

### CoreEngineConfig 변경

```cpp
struct CoreEngineConfig
{
    uint32_t num_cores{0};
    size_t spsc_queue_capacity{1024};               // 신규: SPSC 큐 용량
    // size_t mpsc_queue_capacity{65536};            // 제거
    std::chrono::milliseconds tick_interval{100};
    size_t drain_batch_limit{1024};
};
```

### CoreContext 변경

```cpp
struct CoreContext {
    uint32_t core_id;
    boost::asio::io_context io_ctx{1};
    // MpscQueue<CoreMessage> inbox 제거 — SpscMesh가 대체
    std::unique_ptr<boost::asio::steady_timer> tick_timer;
    CoreMetrics metrics;
};
```

### CoreEngine 변경

```cpp
class CoreEngine {
public:
    /// 코어→코어 메시지 전달 (awaitable). source_core는 thread-local에서 취득.
    /// 비코어 스레드에서 호출 금지 (assert).
    boost::asio::awaitable<void> post_to(uint32_t target_core, CoreMessage msg);

    /// 비코어 스레드용: 타겟 코어의 io_context 직접 접근
    boost::asio::io_context& io_context(uint32_t core_id);

private:
    std::vector<std::unique_ptr<CoreContext>> cores_;
    std::unique_ptr<SpscMesh> mesh_;
};
```

`post_to()` 내부에서 `current_core_id()` (thread-local)로 source_core를 취득한다. 비코어 스레드(`tls_core_id_ == UINT32_MAX`)에서 호출 시 assert 실패.

### Cross-Core API 전환

```cpp
// cross_core_post_msg: 코어→코어 전용, awaitable
inline boost::asio::awaitable<void> cross_core_post_msg(
    CoreEngine& engine, uint32_t source_core, uint32_t target_core,
    CrossCoreOp op, void* data = nullptr)
{
    CoreMessage msg{.op = op, .source_core = source_core,
                    .data = reinterpret_cast<uintptr_t>(data)};
    co_await engine.post_to(target_core, msg);
}

// cross_core_post: 코어→코어 전용, awaitable (레거시 클로저)
template <typename F>
boost::asio::awaitable<void> cross_core_post(
    CoreEngine& engine, uint32_t target_core, F&& func)
{
    auto* task = new std::function<void()>(std::forward<F>(func));
    CoreMessage msg{.op = CrossCoreOp::LegacyCrossCoreFn,
                    .data = reinterpret_cast<uintptr_t>(task)};
    co_await engine.post_to(target_core, msg);
}

// broadcast_cross_core: 코어→전코어, awaitable
inline boost::asio::awaitable<void> broadcast_cross_core(
    CoreEngine& engine, uint32_t source_core,
    CrossCoreOp op, SharedPayload* payload)
{
    if (engine.core_count() <= 1) { delete payload; co_return; }

    for (uint32_t i = 0; i < engine.core_count(); ++i) {
        if (i == source_core) continue;
        CoreMessage msg{.op = op, .source_core = source_core,
                        .data = reinterpret_cast<uintptr_t>(payload)};
        co_await engine.post_to(i, msg);
    }
}

// cross_core_call: 내부 post_to() 호출만 co_await 전환 (반환 타입 불변)
```

### Caller 영향

| Caller 유형 | 전환 내용 |
|------------|----------|
| 서비스 핸들러 (이미 코루틴) | `co_await` 추가만으로 완료 |
| `BroadcastFanout::fanout()` (Redis 스레드) | `cross_core_post()` → `asio::post(engine.io_context(core_id), callback)` |
| 테스트 코드 | `asio::post()` 또는 코루틴 테스트 헬퍼 사용 |
| `cross_core_call()` | 내부 `post_to()` co_await 전환 |

**구현 노트 (v0.5.10.0)**: 실제 구현에서 `post_to()`는 동기 `Result<void>`로 유지하여 비코어 스레드(테스트, 어댑터 콜백) 호환성을 확보했다. awaitable 경로는 별도 `co_post_to()`로 분리. 고수준 API(`cross_core_post_msg`, `cross_core_post`, `broadcast_cross_core`)는 설계대로 awaitable로 전환되었다.

## 7. 벤치마크 및 테스트

### 단위 테스트

**`test_spsc_queue.cpp`:**
- 기본 FIFO, 용량 경계, wrap-around, empty/size
- await backpressure: full → suspend → drain → resume → 성공
- lost wakeup 방지: re-check 프로토콜 검증
- cancel_waiting_producer: 셧다운 시 정리
- TSAN: producer/consumer 별도 스레드 동시 동작

**`test_spsc_mesh.cpp`:**
- 메시 구조: queue(src, dst), src==dst assert
- 단일 코어 모드 (N=1): 큐 0개, drain 즉시 반환
- drain_all_for: 다중 소스 메시지 수신 확인
- drain coalescing: 다중 post → 단일 drain 병합
- 셧다운: 잔여 메시지 + 대기 producer 정리
- CoreEngine 통합: awaitable post_to() + dispatcher 호출

### 마이크로 벤치마크

**`bench_spsc_queue.cpp`:**

| 벤치마크 | 측정 대상 |
|---------|----------|
| BM_SpscQueue_Throughput | 1P:1C 최대 처리량 (vs MpscQueue 1P1C) |
| BM_SpscQueue_Latency | enqueue→dequeue 단방향 지연 |
| BM_SpscQueue_Backpressure | full 큐 await resume 지연 |

### 통합 벤치마크

**기존 확장:**
- `bench_cross_core_latency.cpp` — MPSC vs SPSC ping-pong RTT 비교
- `bench_cross_core_message_passing.cpp` — MPSC vs SPSC throughput 비교

**신규:**
- `bench_spsc_mesh_contention.cpp` — N코어 동시 전송 worst-case. 코어 수 4/8/16/32/48 스케일링 곡선. MPSC CAS contention vs SPSC wait-free 정량 비교.

## 8. 파일 변경 목록

### 신규 파일

| 파일 | 설명 |
|------|------|
| `apex_core/include/apex/core/spsc_queue.hpp` | SpscQueue\<T\> 구현 (header-only) |
| `apex_core/include/apex/core/spsc_mesh.hpp` | SpscMesh 클래스 |
| `apex_core/src/spsc_mesh.cpp` | SpscMesh 구현 |
| `apex_core/tests/unit/test_spsc_queue.cpp` | SpscQueue 단위 테스트 |
| `apex_core/tests/unit/test_spsc_mesh.cpp` | SpscMesh 단위 테스트 |
| `apex_core/benchmarks/micro/bench_spsc_queue.cpp` | SpscQueue 마이크로 벤치마크 |
| `apex_core/benchmarks/integration/bench_spsc_mesh_contention.cpp` | Mesh contention 벤치마크 |

### 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `apex_core/include/apex/core/core_engine.hpp` | CoreContext inbox 제거, CoreEngineConfig 변경, SpscMesh 추가, post_to() awaitable, io_context() 접근자 추가 |
| `apex_core/src/core_engine.cpp` | SpscMesh 초기화, drain 로직 전환, 셧다운 시퀀스 |
| `apex_core/include/apex/core/cross_core_call.hpp` | cross_core_post_msg/post/broadcast awaitable 전환 |
| `apex_services/gateway/src/broadcast_fanout.cpp` | cross_core_post() → asio::post(io_context()) 전환 |
| `apex_core/benchmarks/integration/bench_cross_core_latency.cpp` | SPSC 시나리오 추가 |
| `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp` | SPSC 시나리오 추가 |
| `apex_core/CMakeLists.txt` | 신규 소스/테스트/벤치마크 등록 |
| 서비스 핸들러 (gateway, auth-svc, chat-svc) | cross-core API 호출부 co_await 추가 |
| `apex_core/tests/unit/test_cross_core_call.cpp` | 테스트 코드 awaitable 전환 |

### 보존 파일 (변경 없음)

| 파일 | 사유 |
|------|------|
| `apex_core/include/apex/core/mpsc_queue.hpp` | 유틸리티로 보존, 벤치마크 비교용 |
| `apex_core/tests/unit/test_mpsc_queue.cpp` | 기존 테스트 유지 |
| `apex_core/benchmarks/micro/bench_mpsc_queue.cpp` | 비교 벤치마크로 유지 |
