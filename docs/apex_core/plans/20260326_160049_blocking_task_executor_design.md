# BlockingTaskExecutor 설계 — BACKLOG-146

> AuthService bcrypt 검증이 코어 스레드에서 동기 블로킹되는 문제를 해결하기 위해,
> 코어 프레임워크에 CPU-bound 작업 offload 메커니즘을 추가한다.

## 1. 문제

`auth_service.cpp:177`에서 `password_hasher_.verify()`가 bcrypt(work factor 12, ~250ms)를
코어 IO 스레드에서 동기 실행. 해당 코어의 모든 비동기 작업이 250ms 블로킹됨.

```
on_login() 코루틴 → co_await pg_->query() → password_hasher_.verify() ← 블로킹
                                               ↑ 이 동안 코어 스레드 점유
```

## 2. 해결 방향

코어 프레임워크에 `BlockingTaskExecutor`를 추가하여 CPU-bound 작업을 별도 스레드 풀로 offload.
코루틴은 offload 시점에 suspend되어 코어 스레드를 반환하고, 작업 완료 후 원래 코어 스레드에서 resume.

```
코어 스레드 (io_context)              thread pool
─────────────────────────             ──────────────
on_login() 코루틴 실행 중
  │
  ├─ co_await blocking_executor().run(...)
  │    → 작업을 thread pool에 post ──────────→  bcrypt 연산 시작
  │    → 코루틴 suspend (코어 스레드 반환)        (~250ms CPU 작업)
  │                                              │
  ├─ 다른 코루틴/I/O 처리                          │
  │   ...                                        │
  │                                        작업 완료
  │    ← 결과를 코어 io_context에 post ←──────────┘
  │
  ├─ on_login() 코루틴 resume
  ▼
```

## 3. 설계 결정

| 결정 항목 | 선택 | 근거 |
|-----------|------|------|
| 풀 소유권 | Server 레벨 단일 풀 | CPU offload는 공유해도 shared-nothing 침범 없음. 자원 효율적 |
| 접근 방식 | ServiceBase 접근자 | 기존 패턴(spawn, post, get_executor)과 일관 |
| 스레드 수 | ServerConfig 설정 (기본 2) | 간헐적 사용이라 상시 점유 아님. 서비스별 튜닝 가능 |
| API | awaitable 템플릿 | co_await로 자연스럽게 통합, 반환값 전달 |

## 4. 컴포넌트 설계

### 4.1 BlockingTaskExecutor

**파일**: `apex_core/include/apex/core/blocking_task_executor.hpp`

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/this_coro.hpp>

#include <type_traits>

namespace apex::core {

class BlockingTaskExecutor
{
  public:
    explicit BlockingTaskExecutor(uint32_t thread_count)
        : pool_(thread_count)
    {}

    ~BlockingTaskExecutor() { shutdown(); }

    BlockingTaskExecutor(const BlockingTaskExecutor&) = delete;
    BlockingTaskExecutor& operator=(const BlockingTaskExecutor&) = delete;

    /// CPU-bound 작업을 스레드 풀로 offload하고 결과를 호출자 코어로 반환
    template <typename F>
    auto run(F&& fn) -> boost::asio::awaitable<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;

        auto caller_executor = co_await boost::asio::this_coro::executor;

        // thread pool에서 실행, 완료 후 caller executor로 resume
        co_return co_await boost::asio::co_spawn(
            pool_,
            [f = std::forward<F>(fn)]() -> boost::asio::awaitable<R> {
                co_return f();
            },
            boost::asio::use_awaitable_t<decltype(caller_executor)>{});
    }

    void shutdown()
    {
        pool_.join();
    }

  private:
    boost::asio::thread_pool pool_;
};

}  // namespace apex::core
```

**핵심 메커니즘**:
- `co_await this_coro::executor` — 호출자의 코어 executor 캡처
- `co_spawn(pool_, ...)` — 작업을 thread pool executor에서 실행
- `use_awaitable_t<caller_executor_type>` — 완료 시 호출자 executor로 resume
- 템플릿이라 반환 타입 자동 추론 (`bool`, `std::string` 등)

### 4.2 ServerConfig 확장

**파일**: `apex_core/include/apex/core/server_config.hpp`

```cpp
uint32_t blocking_pool_threads = 2;  // BlockingTaskExecutor 스레드 수
```

기본값 2: bcrypt 등 CPU offload는 간헐적 요청이라 2개면 충분. 부하 테스트 후 조정 가능.

### 4.3 Server 통합

**파일**: `apex_core/include/apex/core/server.hpp`, `apex_core/src/server.cpp`

- `Server` 멤버: `std::unique_ptr<BlockingTaskExecutor> blocking_executor_`
- 생성 시점: `Server::run()` 초기, CoreEngine 시작 전
- 접근자: `BlockingTaskExecutor& blocking_executor()`
- Shutdown: `finalize_shutdown()` 내 CoreEngine stop 이후, Adapter close 이전

### 4.4 ServiceBase 접근자

**파일**: `apex_core/include/apex/core/service_base.hpp`

```cpp
[[nodiscard]] BlockingTaskExecutor& blocking_executor() noexcept
{
    assert(blocking_executor_ && "blocking_executor() called before internal_configure");
    return *blocking_executor_;
}
```

- `BlockingTaskExecutor*` 포인터를 `internal_configure()` 시점에 Server가 주입
- 기존 `io_ctx_` 주입 패턴과 동일

## 5. AuthService 수정

**파일**: `apex_services/auth-svc/src/auth_service.cpp`

### 5.1 on_login() — verify offload (Line 177)

변경 전:
```cpp
auto bcrypt_ok = password_hasher_.verify(password, password_hash);
```

변경 후:
```cpp
auto bcrypt_ok = co_await blocking_executor().run([&] {
    return password_hasher_.verify(password, password_hash);
});
```

### 5.2 on_start() — hash offload (테스트 유저 시딩)

변경 전:
```cpp
spawn([this]() -> boost::asio::awaitable<void> {
    auto hash = password_hasher_.hash("password123");
    // ...
});
```

변경 후:
```cpp
spawn([this]() -> boost::asio::awaitable<void> {
    auto hash = co_await blocking_executor().run([this] {
        return password_hasher_.hash("password123");
    });
    // ...
});
```

## 6. 테스트

**파일**: `apex_core/tests/unit/test_blocking_task_executor.cpp`

| 테스트 케이스 | 검증 내용 |
|--------------|----------|
| 기본 실행 | `run()`이 값을 정상 반환 |
| void 반환 | void 작업 offload |
| 동시 실행 | 복수 코루틴이 동시에 offload |
| executor 복귀 | resume이 호출자 executor에서 일어남 |
| 예외 전파 | 작업 내 예외가 호출자에게 전파 |

## 7. 변경 파일 목록

| 파일 | 변경 유형 |
|------|----------|
| `apex_core/include/apex/core/blocking_task_executor.hpp` | **신규** |
| `apex_core/include/apex/core/server_config.hpp` | 수정 — 필드 추가 |
| `apex_core/include/apex/core/server.hpp` | 수정 — 멤버 + 접근자 |
| `apex_core/src/server.cpp` | 수정 — 생성/shutdown 통합 |
| `apex_core/include/apex/core/service_base.hpp` | 수정 — 접근자 + 포인터 |
| `apex_core/src/service_base.cpp` | 수정 — 포인터 초기화 (있는 경우) |
| `apex_core/CMakeLists.txt` | 수정 — 헤더 등록 (있는 경우) |
| `apex_services/auth-svc/src/auth_service.cpp` | 수정 — offload 적용 |
| `apex_core/tests/unit/test_blocking_task_executor.cpp` | **신규** |
| `apex_core/tests/unit/CMakeLists.txt` | 수정 — 테스트 등록 |

## 8. 설계 원칙 준수

- **shared-nothing**: thread pool은 순수 CPU 연산용이라 I/O 경로·세션 상태와 무관. 코어 간 상태 공유 없음
- **per-core 독립**: 결과가 반드시 호출자의 코어 executor로 돌아옴. 다른 코어의 상태에 접근하지 않음
- **intrusive_ptr 수명**: 해당 없음 (bcrypt 연산은 스칼라 값만 다룸)
- **재작업 방지**: 프레임워크 레벨 유틸리티로 제공하여 향후 JWT 서명·암호화 등에서 재사용
