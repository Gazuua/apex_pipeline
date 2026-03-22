# BACKLOG-132: RedisAdapter::do_close() UAF 방어 — cancellation 인프라 + shutdown 재배치

**버전**: v0.5.10.6
**스코프**: shared (AdapterBase, RedisAdapter, RedisMultiplexer), core (Server shutdown)
**연관**: #24, #29, #129 (HISTORY), #132

---

## 1. 문제

`RedisAdapter::do_close()`가 `per_core_.clear()`로 RedisMultiplexer를 **동기 파괴**하지만, `close()` 코루틴을 호출하지 않는다.

RedisMultiplexer에는 detached 코루틴 2종이 존재한다:
- **reconnect_loop** — 재연결 시도 루프 (`co_spawn(..., detached)`)
- **AUTH 코루틴** — 초기 인증 (`co_spawn(lambda capturing this, detached)`)

둘 다 multiplexer 멤버(`reconnecting_`, `backoff_timer_`, `conn_` 등)를 직접 참조한다.

### 현재 안전성 — "우연히 안전"

```
finalize_shutdown():
  Step 5: CoreEngine stop/join   ← io_context 정지 (코루틴 실행 불가)
  Step 6: adapter->close()       ← per_core_.clear() (이미 죽은 io_context)
```

io_context가 먼저 정지되므로 detached 코루틴이 재개될 수 없어 사실상 안전하지만, Boost.Asio의 암묵적 동작에 의존하는 것이지 명시적 보장이 아니다.

### 방향 전환 이력

| # | 방안 | 결과 |
|---|------|------|
| A | `do_close()` → awaitable로 변경 | 기각 (인터페이스 변경 + io_context 이미 정지) |
| B | shutdown 순서 재배치 (close → stop) | 확정 후 UAF 발견 — io_context가 살아있는 동안 detached 코루틴이 파괴된 멤버 접근 |
| **최종** | **cancellation 인프라 선행 → 코루틴 명시 취소 → 재배치 가능** | 이번 작업에서 구현 |

---

## 2. 설계 결정 요약

| 결정 축 | 선택 |
|---------|------|
| 목표 수준 | AdapterBase 범용 인프라 (RedisAdapter뿐 아니라 전체 어댑터) |
| 취소 메커니즘 | `boost::asio::cancellation_signal` + `CancellationToken` 래퍼 |
| API 형태 | `spawn_adapter_coro` + `cancel_all_coros` + `wait_all_coros`, `drain()` 기본 구현이 cancel 강제 |
| per-core 소유권 | AdapterBase가 per-core CancellationToken 배열 소유 |
| 대기 통합 | 기존 Step 4.5 폴링 루프에 어댑터 카운터 합산 |
| shutdown 재배치 | 이번 PR에서 함께 수행 |

---

## 3. CancellationToken — 핵심 프리미티브

### 역할

per-core 1개씩 존재하며, 해당 코어에서 spawn된 어댑터 코루틴의 추적과 취소를 담당한다.

### 구조

```cpp
// apex_shared/lib/adapters/common/include/apex/shared/adapters/cancellation_token.hpp

class CancellationToken {
    struct Slot {
        boost::asio::cancellation_signal signal;
    };

    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic<uint32_t> outstanding_{0};

#ifndef NDEBUG
    std::thread::id owner_thread_{};
    void assert_owner_thread();  // 첫 호출에서 캡처, 이후 검증
#else
    void assert_owner_thread() {}
#endif

public:
    boost::asio::cancellation_slot new_slot();   // signal 생성 + 카운터 증가
    void cancel_all();                            // 모든 signal에 terminal emit
    void on_complete();                           // 코루틴 완료 시 카운터 감소
    uint32_t outstanding() const;                 // atomic read, 크로스 스레드 안전
};
```

### 스레드 안전성 모델

| 연산 | 실행 위치 | 강제 메커니즘 |
|------|----------|--------------|
| `new_slot()` | 소유 코어 스레드 | debug assert (스레드 ID 비교) |
| `cancel_all()` | `post()`로 소유 코어에 전달 | debug assert |
| `on_complete()` | 소유 코어 스레드 (코루틴 종료 시) | debug assert |
| `outstanding()` | 어느 스레드든 (폴링 루프) | `std::atomic` |

`slots_` 벡터와 `signal::emit()`은 절대 크로스 스레드 접근하지 않는다. 유일한 크로스 스레드 접근은 `outstanding_` atomic 카운터 읽기뿐이다.

`cancel_all()`과 `new_slot()` 등의 스레드 소유권은 debug assert가 **검출** 메커니즘이고, **강제** 메커니즘은 프레임워크 구조 자체다 — `cancel_all_coros()`가 `post()`로 올바른 코어에 전달하므로, 올바르게 사용하면 assert가 발동할 일이 없다. assert는 잘못된 사용을 조기에 잡는 안전망.

### 슬롯 생명주기

`slots_` 벡터는 단조 증가한다 (코루틴 완료 시 슬롯을 제거하지 않음). 이는 의도된 설계:
- 어댑터당 코루틴 수가 극히 적음 (RedisMultiplexer: 최대 2-3개)
- 코어 수 × 코루틴 수 = 실질적 상한 (예: 8코어 × 3 = 24 슬롯)
- 슬롯 재사용/정리의 복잡도가 메모리 절약 대비 과도
- 향후 코루틴 수가 크게 증가하는 어댑터가 등장하면 슬롯 풀링으로 확장 가능

### cancellation 전파 경로

`cancellation_signal::emit(terminal)` → slot에 연결된 async operation의 cancellation handler → 내부적으로 `timer::cancel()` 호출 → `operation_aborted` 전달. Boost.Asio의 per-operation cancellation 메커니즘을 통해 동작하며, `steady_timer::async_wait`는 terminal cancellation을 지원한다. 다른 async operation으로 교체 시 해당 operation의 cancellation 지원 여부를 반드시 확인해야 한다.

### 사용 패턴

```cpp
// 기존: co_spawn(io_ctx, reconnect_loop(), detached);
// 변경: spawn_adapter_coro(core_id, reconnect_loop());
// 내부: co_spawn(io_ctx, wrap_with_guard(token.new_slot(), coro), detached);
```

`wrap_with_guard`는 코루틴을 감싸서 종료 시 (정상/취소/예외 모두) `token.on_complete()`를 호출한다.

---

## 4. AdapterBase 인프라 확장

### 초기화 순서

`AdapterBase::init()`은 `tokens_`와 `io_ctxs_`를 **`do_init()` 호출 전에** 초기화한다. 이것이 보장되어야 `do_init()` 내에서 `spawn_adapter_coro()`를 즉시 사용할 수 있다 (RedisAdapter의 `connect()` → AUTH 코루틴).

```cpp
void init(apex::core::CoreEngine& engine) {
    // 1. 인프라 초기화 (파생 클래스보다 먼저)
    tokens_.resize(engine.core_count());
    io_ctxs_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i)
        io_ctxs_.push_back(&engine.io_context(i));
    // 2. 파생 클래스 초기화 (spawn_adapter_coro 사용 가능)
    do_init(engine);
}
```

### 새 멤버

```cpp
template <typename Derived>
class AdapterBase {
    std::vector<CancellationToken> tokens_;           // per-core, core_count 크기
    std::vector<boost::asio::io_context*> io_ctxs_;   // post 대상
    apex::core::CoreEngine* engine_{nullptr};          // init()에서 캐시, drain/close에서 사용
};
```

`engine_`을 `init()` 시점에 캐시함으로써 `drain()`, `close()` 시그니처를 변경하지 않는다. `io_ctxs_`를 이미 `init()`에서 캐시하고 있으므로 같은 패턴의 자연스러운 확장이다.

### 새 메서드

| 메서드 | 접근 | 용도 |
|--------|------|------|
| `spawn_adapter_coro(core_id, awaitable)` | protected | 파생 어댑터가 코루틴 spawn 시 사용. **DRAINING/CLOSED 상태에서 호출 시 spawn 거부** (로그 경고 + 코루틴 미실행) — drain 이후 새 코루틴 생성 차단 |
| `cancel_all_coros(CoreEngine&)` | private | drain()에서 자동 호출. 각 코어에 post → cancel |
| `outstanding_adapter_coros()` | public | 폴링 루프에서 카운터 합산 |

### drain() 흐름

`engine_`을 멤버로 캐시했으므로 `drain()` 시그니처는 변경하지 않는다.

```cpp
void drain() {
    state_ = AdapterState::DRAINING;
    do_drain();                  // 1. 파생 클래스 추가 로직
    cancel_all_coros();          // 2. AdapterBase 강제 — 내부에서 engine_->io_context(i) 사용
}
```

`cancel_all_coros()`는 `drain()` 공개 메서드 안에서 호출되므로 파생 클래스가 누락할 수 없다.

### AdapterInterface 변경

```cpp
class AdapterInterface {
public:
    virtual void init(CoreEngine& engine) = 0;
    virtual void drain() = 0;                                   // 시그니처 유지
    virtual void close() = 0;
    virtual uint32_t outstanding_adapter_coros() const = 0;     // 신규
};
```

`drain()` 시그니처 유지로 KafkaAdapter, PgAdapter의 변경이 최소화된다. `outstanding_adapter_coros()` 추가만 필요.

---

## 5. RedisMultiplexer 리팩토링

### 제거 대상

- `reconnecting_` 멤버 — `cancellation_signal`이 대체
- 소멸자의 cleanup 로직 — close()가 보장되므로 불필요

### reconnect_loop 변경

```cpp
// 현재: while (reconnecting_) + 플래그 재체크
// 변경: cancellation slot 바인딩, operation_aborted로 종료

boost::asio::awaitable<void> RedisMultiplexer::reconnect_loop()
{
    for (;;) {
        // ... 연결 시도 ...
        auto [ec] = co_await backoff_timer_.async_wait(as_tuple(use_awaitable));
        if (ec == boost::asio::error::operation_aborted)
            co_return;
    }
}
```

플래그 체크 루프 → signal이 co_await를 직접 중단. 반응 지연 0.

### AUTH 코루틴 변경

detached co_spawn → `spawn_adapter_coro(core_id, mux->auth_coro())`로 교체. 추적 대상에 포함.

`connect()`에 `core_id` 파라미터를 추가하여 `spawn_adapter_coro()` 호출이 가능하도록 한다. `RedisAdapter::do_init()`에서 `mux->connect(i)`로 코어 ID를 전달.

### on_disconnect() — drain 이후 spawn 차단

`on_disconnect()` 콜백이 drain 이후에도 hiredis에 의해 호출될 수 있다. 현재는 `on_disconnect()` → `reconnect_loop()` co_spawn이 detached로 실행되는데, drain 이후 새 코루틴이 추적 밖에서 생성되면 원래 문제가 재현된다.

해결: `on_disconnect()`에서 `reconnect_loop()`를 `spawn_adapter_coro()`로 spawn한다. `spawn_adapter_coro()`는 DRAINING/CLOSED 상태에서 spawn을 거부하므로, drain 이후 새 코루틴이 생성될 수 없다.

```cpp
void RedisMultiplexer::on_disconnect()
{
    conn_.reset();
    // spawn_adapter_coro()가 상태 체크 → DRAINING이면 spawn 거부
    adapter_.spawn_adapter_coro(core_id_, reconnect_loop());
}
```

이를 위해 RedisMultiplexer가 소유 RedisAdapter에 대한 참조와 자신의 core_id를 알아야 한다. 생성자 파라미터로 전달.

### close() 변경

`close()`는 **동기 함수로 변경**한다. 현재 awaitable이지만 내부에 `co_await`가 없으므로 awaitable일 필요가 없다. 동기로 전환하면 Section 6의 `do_close_per_core()` 호출과 자연스럽게 매핑된다.

```cpp
void RedisMultiplexer::close()
{
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
    if (conn_) {
        conn_->disconnect();
        conn_.reset();
    }
}
```

`reconnecting_ = false`와 `backoff_timer_.cancel()` 제거 — drain에서 cancellation_signal이 이미 처리.

### 소멸자 변경

```cpp
RedisMultiplexer::~RedisMultiplexer()
{
    APEX_ASSERT(!conn_, "RedisMultiplexer destroyed with active connection — close() not called?");
}
```

cleanup → invariant 검증으로 전환. `APEX_ASSERT`는 Release에서도 활성.

---

## 6. Shutdown 순서 재배치

### 현재 → 변경

```
현재:                                    변경 후:
1. Listener stop                         1. Listener stop
2. Adapter drain                         2. Adapter drain + cancel_all_coros()
3. Scheduler stop                        3. Scheduler stop
4. Service stop                          4. Service stop
4.5. Outstanding coro wait               4.5. Outstanding coro wait
     (서비스 + 인프라)                         (서비스 + 인프라 + 어댑터)
5. CoreEngine stop/join/drain            5. Adapter close  ← 이동 (io_context 활성)
6. Adapter close                         6. CoreEngine stop/join/drain
7. Globals clear                         7. Globals clear
```

### 각 단계 invariant

| 단계 | 선행 조건 |
|------|----------|
| Step 2 cancel | Listener 정지 → 새 세션 없음. DRAINING → 새 요청 거부 |
| Step 4.5 | cancel 발행 → 어댑터 코루틴 operation_aborted로 종료 중 |
| Step 5 (new) | outstanding 전부 0 → 모든 코루틴 종료 → close() 안전 |
| Step 6 | 어댑터 리소스 정리 완료 → io_context에 어댑터 참조 핸들러 없음 → stop 안전 |

### close() 실행 방식 — 2단계 구조

`close()`는 2단계로 동작한다:
1. **per-core cleanup** (`do_close_per_core`) — 각 코어의 io_context에 post. 기본 구현은 no-op. per-core 리소스가 있는 어댑터(Redis)만 override.
2. **전역 cleanup** (`do_close`) — control 스레드에서 동기 호출. 기존 패턴 유지. 전역 리소스가 있는 어댑터(Kafka producer flush)가 사용.

이 2단계 구조로 KafkaAdapter의 전역 producer flush와 RedisAdapter의 per-core multiplexer close가 모두 자연스럽게 처리된다.

```cpp
// AdapterBase::close() — finalize_shutdown()의 control 스레드에서 호출
void close() {
    state_ = AdapterState::CLOSED;

    // Phase 1: per-core cleanup (io_context에 post)
    std::atomic<uint32_t> remaining{static_cast<uint32_t>(io_ctxs_.size())};
    for (uint32_t i = 0; i < io_ctxs_.size(); ++i) {
        boost::asio::post(*io_ctxs_[i], [this, i, &remaining] {
            do_close_per_core(i);     // 파생 override 또는 기본 no-op
            remaining.fetch_sub(1, std::memory_order_release);
        });
    }
    while (remaining.load(std::memory_order_acquire) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});

    // Phase 2: 전역 cleanup (control 스레드에서 동기)
    do_close();                       // 기존 패턴 유지
}

// RedisAdapter — per-core override
void do_close_per_core(uint32_t core_id) {
    per_core_[core_id]->close();  // 동기, 해당 코어 스레드
}
void do_close() { /* per-core에서 이미 처리, 로그만 */ }

// KafkaAdapter — 전역 override (per-core는 기본 no-op 사용)
void do_close() {
    producer_->flush();  // 전역 리소스 정리
    consumers_.clear();
    producer_.reset();
}
```

### spin-wait 안전성

close()는 control 스레드에서 호출되며, 이 시점에서 control_io_는 더 이상 처리할 핸들러가 없다 (finalize_shutdown은 control_io_ 위의 마지막 작업이고, close() 이후 control_io_.stop()이 호출됨).

**코어 스레드 활성 보장**: CoreEngine은 `boost::asio::executor_work_guard`를 보유하여 명시적 `stop()` 전까지 `io_context::run()`이 리턴하지 않는다. Step 4.5에서 모든 코루틴이 종료되어도 work_guard가 스레드를 유지하므로, Step 5에서 `post()`된 핸들러는 반드시 처리된다. CoreEngine stop은 Step 6에서 수행.

### invariant 주석 (코드에 삽입)

```cpp
// Step 5: Adapter close
// INVARIANT: outstanding_adapter_coros == 0 (Step 4.5에서 확인됨)
// INVARIANT: io_context 아직 실행 중 (CoreEngine stop은 Step 6)
// INVARIANT: 새 요청 없음 (Step 2에서 DRAINING)
// WARNING: Step 6 이후로 이동 금지 — close()가 per-core io_context에서 실행됨
```

---

## 7. 테스트 전략

### 단위 테스트

| 테스트 | 검증 대상 |
|--------|----------|
| `CancellationToken_spawn_and_cancel` | spawn → cancel_all → outstanding 0 수렴 |
| `CancellationToken_thread_ownership` | 다른 스레드에서 new_slot() 호출 시 assert 발동 (debug) |
| `CancellationToken_multiple_coros` | 여러 코루틴 spawn → cancel → 전부 종료 확인 |
| `CancellationToken_cancel_idempotent` | cancel_all 중복 호출 안전성 |
| `RedisMultiplexer_close_cancels_reconnect` | 연결 실패 상태에서 cancel → reconnect_loop 종료 확인 |
| `RedisMultiplexer_destructor_assert` | close() 없이 파괴 시 APEX_ASSERT 발동 |

| `AdapterBase_spawn_rejected_after_drain` | DRAINING 상태에서 spawn_adapter_coro 거부 확인 |
| `Shutdown_adapter_close_before_engine_stop` | shutdown 시퀀스에서 adapter close가 CoreEngine stop 이전에 실행되는지 확인 |

### 통합/회귀 테스트

- 기존 E2E 11개 정상 통과 (shutdown 순서 변경 regression)
- Redis 연결 실패 상태에서 graceful shutdown이 drain_timeout 내 완료
- reconnect_loop 활성 중 shutdown → drain_timeout 내 정상 종료 (hang 없음)

### Sanitizer

- ASAN: close 후 멤버 접근 시 UAF 탐지
- TSAN: cross-thread signal emit 누락 시 data race 탐지
- 둘 다 CI에서 이미 활성, 별도 설정 불필요

---

## 8. 다른 어댑터 영향

| 어댑터 | 변경 내용 |
|--------|----------|
| KafkaAdapter | detached 코루틴 없음. `drain()` 시그니처 유지. `outstanding_adapter_coros()` 추가 (기본 0 반환). `do_close()`의 전역 producer flush 로직 유지 — `do_close_per_core()`는 기본 no-op 사용 |
| PgAdapter | detached 코루틴 없음. `drain()` 시그니처 유지. `outstanding_adapter_coros()` 추가. `do_close_per_core()`로 per-core pool close 이전 가능하나, 이번 PR에서는 기존 `do_close()` 유지 (리팩토링 스코프 최소화) |
| PostgreSQLAdapter | 미구현 (v0.6). 구현 시 AdapterBase 인프라 자연스럽게 사용 |

---

## 9. 파일 변경 범위 (예상)

| 파일 | 변경 |
|------|------|
| `apex_shared/.../cancellation_token.hpp` | 신규 |
| `apex_shared/.../adapter_base.hpp` | 확장 (tokens_, spawn, cancel, wait) |
| `apex_core/.../adapter_interface.hpp` | 시그니처 변경 |
| `apex_core/src/server.cpp` | shutdown 순서 재배치 + 폴링 루프 확장 |
| `apex_shared/.../redis_adapter.cpp` | spawn_adapter_coro 사용, do_close → per-core |
| `apex_shared/.../redis_multiplexer.cpp` | reconnecting_ 제거, signal 기반 전환 |
| `apex_shared/.../redis_multiplexer.hpp` | 멤버 변경 (reconnecting_ 제거, core_id_/adapter_ 추가, close() 동기화) |
| `apex_shared/.../kafka_adapter.cpp` | outstanding_adapter_coros() 추가, do_close() 유지 |
| `apex_shared/.../kafka_adapter.hpp` | outstanding_adapter_coros() 선언 |
| `apex_shared/.../pg_adapter.cpp` | outstanding_adapter_coros() 추가 |
| `apex_shared/.../pg_adapter.hpp` | outstanding_adapter_coros() 선언 |
| `apex_shared/tests/...` | 단위 테스트 추가 |
