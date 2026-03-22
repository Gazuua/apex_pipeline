# BACKLOG-132 구현 계획: RedisAdapter Close UAF 방어 + Cancellation 인프라

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** AdapterBase에 per-core CancellationToken 인프라를 구축하고, RedisMultiplexer의 detached 코루틴을 추적/취소 가능하게 만든 뒤, shutdown 순서를 재배치하여 "우연한 안전"을 "설계된 안전"으로 격상.

**Architecture:** CancellationToken(per-core 프리미티브) → AdapterBase(spawn/cancel/wait 인프라) → RedisMultiplexer(signal 기반 전환) → Server(shutdown 재배치). 기존 flag 패턴 완전 교체.

**Tech Stack:** C++23, Boost.Asio 1.84+ (cancellation_signal, co_spawn, steady_timer, bind_cancellation_slot), GTest, CRTP

**Spec:** `docs/apex_shared/plans/20260322_155224_backlog-132-redis-adapter-close-uaf.md`

**주의사항 (리뷰 반영):**
- `engine_` 멤버명은 PgAdapter에 이미 존재하므로 AdapterBase에서는 `base_engine_`으로 명명. PgAdapter의 `engine_`은 `base_engine_`으로 마이그레이션하여 중복 제거.
- `boost::asio::bind_cancellation_slot`의 per-operation cancellation 전파는 Boost 1.78+에서 동작. 프로젝트는 1.84 사용 중이므로 문제 없음.
- RedisMultiplexer 생성자 변경 시 기존 slab 설정(auto_grow, max_total_count 등) 반드시 유지.

---

## 파일 맵

### 신규 생성

| 파일 | 역할 |
|------|------|
| `apex_shared/lib/adapters/common/include/apex/shared/adapters/cancellation_token.hpp` | CancellationToken 클래스 정의 |
| `apex_shared/lib/adapters/common/src/cancellation_token.cpp` | CancellationToken 구현 |
| `apex_shared/tests/unit/test_cancellation_token.cpp` | CancellationToken 단위 테스트 |

### 수정 대상

| 파일 | 변경 내용 |
|------|----------|
| `apex_shared/lib/adapters/common/CMakeLists.txt` | cancellation_token.cpp 소스 추가 |
| `apex_shared/tests/unit/CMakeLists.txt` | test_cancellation_token 테스트 등록 |
| `apex_core/include/apex/core/adapter_interface.hpp` | `outstanding_adapter_coros()` 추가 |
| `apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp` | tokens_, io_ctxs_, base_engine_, spawn/cancel/wait, drain/close/init 확장 |
| `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_multiplexer.hpp` | reconnecting_ 제거, core_id_/adapter_ 추가, close() 동기화 |
| `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp` | reconnect_loop signal 전환, on_disconnect 수정, close 동기화, 소멸자 변경 |
| `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_adapter.hpp` | do_close_per_core 추가 |
| `apex_shared/lib/adapters/redis/src/redis_adapter.cpp` | spawn_adapter_coro 사용, do_close_per_core 구현 |
| `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_adapter.hpp` | outstanding_adapter_coros 선언 |
| `apex_shared/lib/adapters/kafka/src/kafka_adapter.cpp` | outstanding_adapter_coros 구현 (0 반환) |
| `apex_shared/lib/adapters/pg/include/apex/shared/adapters/pg/pg_adapter.hpp` | outstanding_adapter_coros 선언 |
| `apex_shared/lib/adapters/pg/src/pg_adapter.cpp` | outstanding_adapter_coros 구현 (0 반환) |
| `apex_core/src/server.cpp:380-461` | shutdown 재배치 + 폴링 루프 확장 |
| `apex_shared/tests/unit/test_adapter_base.cpp` | AdapterBase 새 인프라 테스트 |

---

## Task 1: CancellationToken — 핵심 프리미티브

**Files:**
- Create: `apex_shared/lib/adapters/common/include/apex/shared/adapters/cancellation_token.hpp`
- Create: `apex_shared/lib/adapters/common/src/cancellation_token.cpp`
- Create: `apex_shared/tests/unit/test_cancellation_token.cpp`
- Modify: `apex_shared/lib/adapters/common/CMakeLists.txt:8` (소스 추가)
- Modify: `apex_shared/tests/unit/CMakeLists.txt` (테스트 등록)

### Step 1: 테스트 파일 생성 — spawn + cancel + outstanding 기본 동작

- [ ] `test_cancellation_token.cpp` 생성. 첫 번째 테스트: 코루틴 spawn 시 outstanding 증가, cancel 후 0으로 수렴.

```cpp
// apex_shared/tests/unit/test_cancellation_token.cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/cancellation_token.hpp>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;

TEST(CancellationToken, SpawnAndCancel)
{
    boost::asio::io_context io;

    CancellationToken token;

    // spawn: outstanding 1 증가
    auto slot = token.new_slot();
    EXPECT_EQ(token.outstanding(), 1);

    // 코루틴 시뮬레이션: slot 바인딩된 타이머 대기
    boost::asio::steady_timer timer(io, std::chrono::hours{1}); // 오래 대기
    bool cancelled = false;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            auto [ec] =
                co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec == boost::asio::error::operation_aborted)
                cancelled = true;
            token.on_complete();
        },
        boost::asio::bind_cancellation_slot(slot, boost::asio::detached));

    // 1틱 실행 — 코루틴 시작, 타이머 대기 진입
    io.run_one();
    EXPECT_EQ(token.outstanding(), 1);

    // cancel
    token.cancel_all();
    io.run(); // 남은 핸들러 소진
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(token.outstanding(), 0);
}

TEST(CancellationToken, MultipleCoros)
{
    boost::asio::io_context io;
    CancellationToken token;

    constexpr int N = 5;
    boost::asio::steady_timer timers[N] = {
        {io, std::chrono::hours{1}}, {io, std::chrono::hours{1}},
        {io, std::chrono::hours{1}}, {io, std::chrono::hours{1}},
        {io, std::chrono::hours{1}},
    };
    int completed = 0;

    for (int i = 0; i < N; ++i)
    {
        auto slot = token.new_slot();
        boost::asio::co_spawn(
            io,
            [&, i]() -> boost::asio::awaitable<void> {
                auto [ec] =
                    co_await timers[i].async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                ++completed;
                token.on_complete();
            },
            boost::asio::bind_cancellation_slot(slot, boost::asio::detached));
    }

    io.run_one_for(std::chrono::milliseconds{10}); // 코루틴들 시작
    EXPECT_EQ(token.outstanding(), N);

    token.cancel_all();
    io.run();
    EXPECT_EQ(completed, N);
    EXPECT_EQ(token.outstanding(), 0);
}

TEST(CancellationToken, CancelIdempotent)
{
    CancellationToken token;
    // cancel_all 빈 토큰 — 크래시 안 됨
    token.cancel_all();
    token.cancel_all();
    EXPECT_EQ(token.outstanding(), 0);
}
```

### Step 2: CancellationToken 헤더 생성

- [ ] `cancellation_token.hpp` 생성.

```cpp
// apex_shared/lib/adapters/common/include/apex/shared/adapters/cancellation_token.hpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <boost/asio/cancellation_signal.hpp>

#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>

namespace apex::shared::adapters
{

/// Per-core cancellation token for adapter coroutines.
/// Tracks outstanding coroutines and provides bulk cancellation.
/// Thread safety: all methods except outstanding() must be called from the owning core thread.
class CancellationToken
{
  public:
    /// Allocates a new cancellation slot and increments outstanding counter.
    /// Returns the slot for binding to co_spawn via bind_cancellation_slot.
    [[nodiscard]] boost::asio::cancellation_slot new_slot();

    /// Emits terminal cancellation to all active slots.
    void cancel_all();

    /// Called by coroutine guard on completion (normal, cancelled, or exception).
    void on_complete();

    /// Returns current outstanding count. Thread-safe (atomic read).
    [[nodiscard]] uint32_t outstanding() const noexcept;

  private:
    struct Slot
    {
        boost::asio::cancellation_signal signal;
    };

    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic<uint32_t> outstanding_{0};

#ifndef NDEBUG
    std::thread::id owner_thread_{};
    void assert_owner_thread();
#else
    void assert_owner_thread() {}
#endif
};

} // namespace apex::shared::adapters
```

### Step 3: CancellationToken 구현 생성

- [ ] `cancellation_token.cpp` 생성.

```cpp
// apex_shared/lib/adapters/common/src/cancellation_token.cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/cancellation_token.hpp>

namespace apex::shared::adapters
{

#ifndef NDEBUG
void CancellationToken::assert_owner_thread()
{
    auto current = std::this_thread::get_id();
    if (owner_thread_ == std::thread::id{})
        owner_thread_ = current;
    else
        assert(owner_thread_ == current && "CancellationToken accessed from wrong thread");
}
#endif

boost::asio::cancellation_slot CancellationToken::new_slot()
{
    assert_owner_thread();
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = slots_.emplace_back(std::make_unique<Slot>());
    return slot->signal.slot();
}

void CancellationToken::cancel_all()
{
    assert_owner_thread();
    for (auto& s : slots_)
        s->signal.emit(boost::asio::cancellation_type::terminal);
}

void CancellationToken::on_complete()
{
    assert_owner_thread();
    auto prev = outstanding_.fetch_sub(1, std::memory_order_release);
    assert(prev > 0 && "on_complete called more times than new_slot");
}

uint32_t CancellationToken::outstanding() const noexcept
{
    return outstanding_.load(std::memory_order_acquire);
}

} // namespace apex::shared::adapters
```

### Step 4: CMake에 소스 + 테스트 등록

- [ ] `apex_shared/lib/adapters/common/CMakeLists.txt`에 `cancellation_token.cpp` 추가.
  - 기존 소스 목록 (`adapter_error.cpp`, `circuit_breaker.cpp`) 바로 아래에 `cancellation_token.cpp` 추가.

- [ ] `apex_shared/tests/unit/CMakeLists.txt`에 테스트 등록.
  - 기존 `apex_shared_add_unit_test()` 패턴 사용.
  - `test_cancellation_token` 이름으로 등록. 링크 대상: `apex_shared_adapters_common`, `Boost::headers`.

### Step 5: 빌드 + 테스트 실행

- [ ] clang-format 실행:

```bash
find apex_shared/lib/adapters/common apex_shared/tests/unit \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
  ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] 빌드 (`run_in_background: true`):

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug
```

- [ ] 테스트 실행:

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug --target test_cancellation_token
```

기대: 3개 테스트 PASS.

### Step 6: 커밋

- [ ] 커밋:

```bash
git add apex_shared/lib/adapters/common/include/apex/shared/adapters/cancellation_token.hpp \
        apex_shared/lib/adapters/common/src/cancellation_token.cpp \
        apex_shared/lib/adapters/common/CMakeLists.txt \
        apex_shared/tests/unit/test_cancellation_token.cpp \
        apex_shared/tests/unit/CMakeLists.txt
git commit -m "feat(shared): BACKLOG-132 CancellationToken per-core 프리미티브 추가"
git push
```

---

## Task 2: AdapterInterface + AdapterBase 확장

**Files:**
- Modify: `apex_core/include/apex/core/adapter_interface.hpp:24-29`
- Modify: `apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp:31-143`
- Modify: `apex_shared/tests/unit/test_adapter_base.cpp`

### Step 1: 테스트 작성 — AdapterBase spawn/cancel/wait

- [ ] `test_adapter_base.cpp`에 테스트 추가. 기존 MockAdapter를 확장하여 spawn_adapter_coro 사용 테스트.

```cpp
// test_adapter_base.cpp에 추가

TEST(AdapterBase, SpawnAndCancelAdapterCoros)
{
    // MockAdapter가 AdapterBase<MockAdapter>를 상속
    // init 후 spawn_adapter_coro로 코루틴 spawn
    // drain 후 outstanding 0 확인
    // ... (기존 MockAdapter 구조에 맞춰 구현)
}

TEST(AdapterBase, SpawnRejectedAfterDrain)
{
    // drain 후 spawn_adapter_coro 호출 시 spawn 거부 확인
}
```

주의: 테스트의 정확한 코드는 기존 MockAdapter 구조를 읽고 맞춰야 함. 핵심 검증 포인트:
- `init()` → `spawn_adapter_coro()` → outstanding 1
- `drain()` → cancel 발행 → outstanding 0
- drain 후 `spawn_adapter_coro()` → 거부 (outstanding 변화 없음)

### Step 2: AdapterInterface에 outstanding_adapter_coros 추가

- [ ] `apex_core/include/apex/core/adapter_interface.hpp`의 순수 가상 메서드 목록에 추가:

```cpp
// line 29 근처, name() 아래에 추가
virtual uint32_t outstanding_adapter_coros() const = 0;
```

### Step 3: AdapterBase 확장 — 멤버 + 메서드

- [ ] `adapter_base.hpp` 수정. 주요 변경:

**includes 추가** (파일 상단):
```cpp
#include <apex/shared/adapters/cancellation_token.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <spdlog/spdlog.h>
#include <thread> // std::this_thread::sleep_for in close()
```

**새 멤버** (state_ 아래에 추가):
```cpp
std::vector<CancellationToken> tokens_;
std::vector<boost::asio::io_context*> io_ctxs_;
apex::core::CoreEngine* base_engine_{nullptr};  // PgAdapter engine_ 충돌 방지
```

**init() 변경** (lines 44-48):
```cpp
void init(apex::core::CoreEngine& engine)
{
    // Phase 1: 인프라 초기화 (파생 클래스보다 먼저)
    base_engine_ = &engine;
    tokens_.resize(engine.core_count());
    io_ctxs_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i)
        io_ctxs_.push_back(&engine.io_context(i));
    // Phase 2: 파생 클래스 초기화
    // NOTE: state를 RUNNING으로 먼저 설정해야 do_init() 내에서 spawn_adapter_coro() 사용 가능.
    // 이 시점에서 다른 스레드가 is_ready()를 조회할 수 있으나, do_init()은 Server::run()
    // 내에서 단일 스레드로 실행되고, 코어 스레드는 아직 시작 전이므로 안전.
    state_.store(AdapterState::RUNNING, std::memory_order_release);
    static_cast<Derived*>(this)->do_init(engine);
}
```

**drain() 변경** (lines 51-55):
```cpp
void drain()
{
    state_.store(AdapterState::DRAINING, std::memory_order_release);
    static_cast<Derived*>(this)->do_drain();
    cancel_all_coros(); // AdapterBase 강제 — 내부에서 base_engine_ 사용
}
```

**close() 변경** (lines 58-62):
```cpp
void close()
{
    state_.store(AdapterState::CLOSED, std::memory_order_release);
    // Phase 1: per-core cleanup
    if (!io_ctxs_.empty())
    {
        std::atomic<uint32_t> remaining{static_cast<uint32_t>(io_ctxs_.size())};
        for (uint32_t i = 0; i < io_ctxs_.size(); ++i)
        {
            boost::asio::post(*io_ctxs_[i], [this, i, &remaining] {
                static_cast<Derived*>(this)->do_close_per_core(i);
                remaining.fetch_sub(1, std::memory_order_release);
            });
        }
        // 타임아웃 방어: 5초 이내에 완료되지 않으면 경고 후 진행
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (remaining.load(std::memory_order_acquire) > 0)
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                spdlog::warn("AdapterBase::close() timed out ({} cores remaining)",
                             remaining.load(std::memory_order_relaxed));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    // Phase 2: 전역 cleanup
    static_cast<Derived*>(this)->do_close();
}
```

**새 protected 메서드:**
```cpp
/// Spawns a coroutine on the specified core with cancellation tracking.
/// Rejects spawn if adapter is not RUNNING (logs warning, does not execute).
void spawn_adapter_coro(uint32_t core_id, boost::asio::awaitable<void> coro)
{
    if (state_.load(std::memory_order_acquire) != AdapterState::RUNNING)
    {
        spdlog::warn("{}: spawn_adapter_coro rejected — adapter state is not RUNNING",
                     static_cast<Derived*>(this)->do_name());
        return;
    }
    auto& token = tokens_[core_id];
    auto slot = token.new_slot();
    boost::asio::co_spawn(
        *io_ctxs_[core_id],
        [&token, c = std::move(coro)]() mutable -> boost::asio::awaitable<void> {
            try
            {
                co_await std::move(c);
            }
            catch (...)
            {
            }
            token.on_complete();
        },
        boost::asio::bind_cancellation_slot(slot, boost::asio::detached));
}

/// Default no-op. Override in derived adapters with per-core resources.
void do_close_per_core([[maybe_unused]] uint32_t core_id) {}
```

**새 private 메서드:**
```cpp
void cancel_all_coros()
{
    for (uint32_t i = 0; i < tokens_.size(); ++i)
    {
        boost::asio::post(*io_ctxs_[i], [this, i] {
            tokens_[i].cancel_all();
        });
    }
}
```

**새 public 메서드:**
```cpp
uint32_t outstanding_adapter_coros() const
{
    uint32_t total = 0;
    for (const auto& t : tokens_)
        total += t.outstanding();
    return total;
}
```

**AdapterWrapper 수정** (lines 92-143):
- `outstanding_adapter_coros()` override 추가:

```cpp
uint32_t outstanding_adapter_coros() const override
{
    return adapter_.outstanding_adapter_coros();
}
```

### Step 4: 빌드 + 테스트

- [ ] clang-format → 빌드 → `test_adapter_base` 실행.

### Step 5: 커밋

- [ ] 변경 파일 커밋 + 푸시:

```
feat(core,shared): BACKLOG-132 AdapterBase cancellation 인프라 확장
```

---

## Task 3: KafkaAdapter + PgAdapter 컴파일 적합

**Files:**
- Modify: `apex_shared/lib/adapters/pg/include/apex/shared/adapters/pg/pg_adapter.hpp`
- Modify: `apex_shared/lib/adapters/pg/src/pg_adapter.cpp`

AdapterBase가 `outstanding_adapter_coros()`, `do_close_per_core()` 기본 구현을 제공하므로, KafkaAdapter/PgAdapter에 명시적 override는 불필요. AdapterWrapper가 AdapterInterface의 pure virtual을 AdapterBase로 위임.

### Step 1: PgAdapter — `engine_` → `base_engine_` 마이그레이션

- [ ] PgAdapter는 자체 `engine_` 멤버(`pg_adapter.hpp:90`)를 보유하고 있어 AdapterBase의 `base_engine_`과 이름이 충돌하지는 않지만, **중복 멤버**가 된다. PgAdapter의 `engine_`을 제거하고 `base_engine_`을 사용하도록 변경:

**`pg_adapter.hpp`**: `apex::core::CoreEngine* engine_{nullptr};` 멤버 제거.

**`pg_adapter.cpp`**: `do_init()` 내 `engine_ = &engine;` 제거 (AdapterBase::init()에서 이미 `base_engine_` 설정). `do_close()`에서 `engine_->` 참조를 `base_engine_->` 로 변경.

### Step 2: 빌드 확인

- [ ] 전체 빌드:

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug
```

### Step 3: 커밋

```
refactor(shared): BACKLOG-132 PgAdapter engine_ 중복 제거 — AdapterBase base_engine_ 사용
```

---

## Task 4: RedisMultiplexer 리팩토링

**Files:**
- Modify: `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_multiplexer.hpp`
- Modify: `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp`

### Step 1: 헤더 수정

- [ ] `redis_multiplexer.hpp` 수정:

**제거:**
- `bool reconnecting_{false};` (line 153)

**추가 (멤버):**
- `uint32_t core_id_{0};` — 소유 코어 ID
- `bool reconnect_active_{false};` — on_disconnect 재진입 방어 (같은 코어 스레드에서만 접근)

**추가 (include):**
- `#include <apex/core/assert.hpp>` — APEX_ASSERT 사용

**생성자 시그니처 변경:**
```cpp
// 기존: RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config);
// 변경:
RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config, uint32_t core_id);
```

**connect 시그니처 변경:**
```cpp
// 기존: void connect();
// 변경: 없음 — connect()는 내부에서 core_id_ 사용
```

실제로 `connect()`는 시그니처 변경 불필요 — `core_id_`를 멤버로 보유하므로 내부에서 직접 사용.

**close 시그니처 변경:**
```cpp
// 기존: boost::asio::awaitable<void> close();
// 변경:
void close();
```

**on_disconnect 변경:**
- 현재: `void on_disconnect();` (private)
- 변경: on_disconnect에서 adapter의 spawn_adapter_coro를 호출해야 하므로, RedisAdapter에 대한 참조가 필요. 두 가지 방법:
  - (A) RedisAdapter& 멤버 추가 — 순환 참조 위험
  - (B) `std::function<void(uint32_t, boost::asio::awaitable<void>)>` 콜백

실용적 선택: **(B) 콜백**. 생성자에서 spawn 콜백을 받아 저장.

```cpp
// 새 멤버:
using SpawnCallback = std::function<void(uint32_t, boost::asio::awaitable<void>)>;
SpawnCallback spawn_callback_; // 생성 시 주입

// 생성자:
RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config,
                 uint32_t core_id, SpawnCallback spawn_cb);
```

### Step 2: 구현 수정 — 생성자 + 소멸자

- [ ] `redis_multiplexer.cpp` 수정:

**생성자** (lines 20-25) — 기존 slab 설정 유지 필수:
```cpp
RedisMultiplexer::RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config,
                                   uint32_t core_id, SpawnCallback spawn_cb)
    : io_ctx_(io_ctx)
    , config_(config)
    , slab_(64, {.auto_grow = true, .grow_chunk_size = {}, .max_total_count = config_.max_pending_commands})
    , backoff_timer_(io_ctx)
    , core_id_(core_id)
    , spawn_callback_(std::move(spawn_cb))
{
}
```

**소멸자** (lines 62-67):
```cpp
RedisMultiplexer::~RedisMultiplexer()
{
    APEX_ASSERT(!conn_, "RedisMultiplexer destroyed with active connection — close() not called?");
}
```

### Step 3: connect() 수정 — AUTH를 spawn_callback 경유

- [ ] `connect()` (lines 27-60) 수정:

AUTH 코루틴 부분 (lines 37-49)을 spawn_callback_ 사용으로 변경:

```cpp
// 기존:
// boost::asio::co_spawn(io_ctx_, [this]() -> ... { authenticate ... }, detached);
// 변경:
if (!config_.password.empty())
{
    spawn_callback_(core_id_,
        [this]() -> boost::asio::awaitable<void> {
            auto result = co_await authenticate(*conn_);
            if (!result.has_value())
            {
                spdlog::warn("RedisMultiplexer: initial AUTH failed, reconnecting");
                conn_.reset();
                on_disconnect();
            }
        }());
}
```

초기 연결 실패 시 reconnect_loop도 spawn_callback_ 경유 (lines 56-58):
```cpp
// 기존: boost::asio::co_spawn(io_ctx_, reconnect_loop(), detached);
// 변경:
reconnect_active_ = true; // on_disconnect 재진입 방지 (spawn 전에 설정)
spawn_callback_(core_id_, reconnect_loop());
```

### Step 4: reconnect_loop() 수정 — flag → ec 체크

- [ ] `reconnect_loop()` (lines 331-373) 수정:

```cpp
boost::asio::awaitable<void> RedisMultiplexer::reconnect_loop()
{
    spdlog::debug("[redis_multiplexer] reconnect_loop started ({}:{})",
                  config_.host, config_.port);
    auto backoff = std::chrono::milliseconds{100};
    const auto max_backoff = config_.reconnect_max_backoff;

    // RAII guard: 코루틴 종료 시(정상/취소/예외) reconnect_active_ 해제
    struct ReconnectGuard
    {
        bool& active;
        ~ReconnectGuard() { active = false; }
    } guard{reconnect_active_};

    for (;;)
    {
        ++reconnect_attempts_;
        spdlog::info("Redis reconnect attempt {} (backoff {}ms)",
                     reconnect_attempts_, backoff.count());

        conn_ = RedisConnection::create(io_ctx_, config_);
        if (conn_ && conn_->is_connected())
        {
            auto auth = co_await authenticate(*conn_);
            if (!auth.has_value())
            {
                conn_.reset();
                backoff_timer_.expires_after(backoff);
                auto [ec] = co_await backoff_timer_.async_wait(
                    boost::asio::as_tuple(boost::asio::use_awaitable));
                if (ec == boost::asio::error::operation_aborted)
                    co_return;
                backoff = std::min(backoff * 2, max_backoff);
                continue;
            }
            reconnect_attempts_ = 0;
            spdlog::info("Redis reconnected successfully ({}:{})",
                         config_.host, config_.port);
            co_return;
        }

        backoff_timer_.expires_after(backoff);
        auto [ec] = co_await backoff_timer_.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec == boost::asio::error::operation_aborted)
            co_return;
        backoff = std::min(backoff * 2, max_backoff);
    }
}
```

### Step 5: on_disconnect() 수정 — spawn_callback 경유 + 재진입 방어

- [ ] `on_disconnect()` (lines 260-272) 수정:

```cpp
void RedisMultiplexer::on_disconnect()
{
    conn_.reset();
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
    spdlog::warn("Redis disconnected ({}:{}), starting reconnect...",
                 config_.host, config_.port);

    // 재진입 방어: reconnect_loop가 이미 활성이면 중복 spawn 방지
    if (reconnect_active_)
        return;
    reconnect_active_ = true;

    // spawn_callback가 DRAINING 상태면 거부 → 새 코루틴 없음
    spawn_callback_(core_id_, reconnect_loop());
}
```

reconnect_loop 종료 시 `reconnect_active_ = false` 설정 필요 — reconnect_loop() 시작부와 종료부에 추가.

### Step 6: close() 동기화

- [ ] `close()` (lines 441-453) 수정:

```cpp
void RedisMultiplexer::close()
{
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
    if (conn_)
    {
        conn_->disconnect();
        conn_.reset();
    }
}
```

`reconnecting_ = false`와 `backoff_timer_.cancel()` 제거.

### Step 7: reconnecting_ 참조 제거 + connected() 수정

- [ ] `reconnecting_` 참조하는 모든 위치 정리:
  - `connect()`: `reconnecting_ = true` 제거
  - `on_disconnect()`: `reconnecting_` 관련 코드 제거 (Step 5에서 이미 처리)
  - `connected()` (lines 426-429): `!reconnecting_` 조건 제거

```cpp
// 기존: return conn_ && conn_->is_connected() && !reconnecting_;
// 변경:
bool RedisMultiplexer::connected() const noexcept
{
    return conn_ && conn_->is_connected();
}
```

  - grep으로 잔여 참조 확인: `grep -rn "reconnecting_" apex_shared/lib/adapters/redis/`

### Step 8: 빌드 + 기존 테스트

- [ ] clang-format → 빌드 → 기존 Redis 테스트 실행:

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug --target test_redis_adapter
```

### Step 9: 커밋

```
refactor(shared): BACKLOG-132 RedisMultiplexer — flag 패턴을 cancellation_signal로 전환
```

---

## Task 5: RedisAdapter 변경

**Files:**
- Modify: `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_adapter.hpp`
- Modify: `apex_shared/lib/adapters/redis/src/redis_adapter.cpp`

### Step 1: 헤더 수정 — do_close_per_core 추가

- [ ] `redis_adapter.hpp` 수정:

`do_close()` 선언 (line 45) 아래에 추가:
```cpp
void do_close_per_core(uint32_t core_id);
```

### Step 2: do_init() 수정 — spawn_callback 주입

- [ ] `redis_adapter.cpp`의 `do_init()` (lines 22-45) 수정:

Multiplexer 생성 시 core_id + spawn_callback 전달:

```cpp
void RedisAdapter::do_init(apex::core::CoreEngine& engine)
{
    // 기존 validation 로직 유지 (host empty → 기존 동작 그대로)
    // 기존 코드에서 throw/return 패턴이 있다면 그대로 유지

    per_core_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i)
    {
        auto mux = std::make_unique<RedisMultiplexer>(
            engine.io_context(i), config_, i,
            [this](uint32_t core_id, boost::asio::awaitable<void> coro) {
                this->spawn_adapter_coro(core_id, std::move(coro));
            });
        mux->connect();
        per_core_.push_back(std::move(mux));
    }

    spdlog::info("RedisAdapter: initialized {} multiplexers ({}:{})",
                 engine.core_count(), config_.host, config_.port);
}
```

### Step 3: do_close() + do_close_per_core() 구현

- [ ] `do_close()` (lines 52-56) 수정:

```cpp
void RedisAdapter::do_close()
{
    per_core_.clear();
    spdlog::info("RedisAdapter: closed");
}

void RedisAdapter::do_close_per_core(uint32_t core_id)
{
    if (core_id < per_core_.size() && per_core_[core_id])
        per_core_[core_id]->close();
}
```

`do_close()`에서 `per_core_.clear()`는 Phase 2(전역 cleanup)에서 실행. Phase 1의 `do_close_per_core()`에서 이미 각 multiplexer의 `close()`를 호출했으므로, `clear()`는 빈 unique_ptr 해제만 수행.

### Step 4: 소멸자 확인

- [ ] `~RedisAdapter()` (lines 17-20) 확인:

기존 소멸자에서 `per_core_` 정리 로직이 있다면 제거하거나 유지 확인. APEX_ASSERT는 multiplexer 소멸자에서 처리하므로 adapter 소멸자는 기본 동작으로 충분.

### Step 5: 빌드 + 테스트

- [ ] clang-format → 빌드 → 테스트:

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug
```

### Step 6: 커밋

```
refactor(shared): BACKLOG-132 RedisAdapter — spawn_adapter_coro 사용 + per-core close
```

---

## Task 6: Server Shutdown 순서 재배치

**Files:**
- Modify: `apex_core/src/server.cpp:380-461`

### Step 1: finalize_shutdown() 수정

- [ ] `server.cpp`의 `finalize_shutdown()` 수정:

**Step 2 (Adapter drain)** — 기존 코드 유지. cancel_all_coros()는 AdapterBase::drain() 내부에서 자동 호출.

**Step 4.5 (Outstanding coro wait)** — 어댑터 카운터 합산:

```cpp
// 기존 폴링 루프에 추가:
// total += core_engine_->outstanding_infra_coroutines(); 아래에:
for (const auto& adapter : adapters_)
    total += adapter->outstanding_adapter_coros();
```

**Step 5 ↔ Step 6 교환:**

```cpp
// Step 5: Adapter close (NEW — 기존 Step 6에서 이동)
// INVARIANT: outstanding_adapter_coros == 0 (Step 4.5에서 확인됨)
// INVARIANT: io_context 아직 실행 중 (CoreEngine stop은 Step 6)
// INVARIANT: 새 요청 없음 (Step 2에서 DRAINING)
// WARNING: Step 6 이후로 이동 금지 — close()가 per-core io_context에서 실행됨
spdlog::info("[shutdown] step 5: closing adapters");
for (auto& adapter : adapters_)
    adapter->close();

// Step 6: CoreEngine stop + join + drain (기존 Step 5)
// INVARIANT: 어댑터 리소스 정리 완료 — io_context에 어댑터 참조 핸들러 없음
spdlog::info("[shutdown] step 6: stopping core engine");
core_engine_->stop();
core_engine_->join();
core_engine_->drain_remaining();
```

### Step 2: 기존 Step 6 위치의 adapter close 제거

- [ ] 기존 Step 6 위치 (lines 445-450)의 adapter close 코드 제거 — Step 5로 이동했으므로.

### Step 3: 빌드

- [ ] clang-format → 빌드:

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug
```

### Step 4: 커밋

```
refactor(core): BACKLOG-132 shutdown 재배치 — adapter close를 CoreEngine stop 이전으로 이동
```

---

## Task 7: 통합 검증

**Files:**
- 전체 빌드 + 테스트 실행
- `docs/BACKLOG.md`

### Step 1: clang-format 최종 확인

- [ ] 전체 포맷:

```bash
find apex_core apex_shared apex_services \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
  ! -name '*_generated.h' | xargs clang-format -i
```

변경 있으면 커밋.

### Step 2: 전체 빌드

- [ ] 전체 debug 빌드 (`run_in_background: true`):

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug
```

### Step 3: 전체 테스트 실행

- [ ] 빌드 후 테스트:

```bash
"$(pwd)/apex_tools/queue-lock.sh" build debug --target test
```

기대: 전체 PASS (기존 71+ 유닛 + 11 E2E + 신규 CancellationToken/AdapterBase 테스트).

### Step 4: BACKLOG.md — #132를 NOW로 이동

- [ ] `docs/BACKLOG.md`에서 #132를 IN VIEW → NOW 섹션으로 이동.

### Step 5: 커밋

```
docs: BACKLOG-132 — NOW로 승격
```

---

## 태스크 의존성 그래프

```
Task 1 (CancellationToken)
    ↓
Task 2 (AdapterBase 확장)
    ↓
Task 3 (Kafka/Pg 적합) ←── Task 2에 의존
    ↓
Task 4 (RedisMultiplexer 리팩토링) ←── Task 2에 의존
    ↓
Task 5 (RedisAdapter 변경) ←── Task 4에 의존
    ↓
Task 6 (Server shutdown 재배치) ←── Task 2에 의존
    ↓
Task 7 (통합 검증) ←── 전체 의존
```

Task 3, 4, 6은 Task 2 완료 후 병렬 가능하지만, Task 4 → Task 5는 순차 의존.
