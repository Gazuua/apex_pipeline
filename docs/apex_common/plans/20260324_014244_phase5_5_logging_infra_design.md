# Phase 5.5 로깅 인프라 설계 — ScopedLogger + 전 레이어 로그 충실화

> **브랜치**: `feature/phase5.5-logging-infra`
> **백로그**: BACKLOG-161 (source_location), BACKLOG-174 (서비스 로그 충실화)
> **스코프**: apex_core, apex_shared, apex_services (Gateway, Auth, Chat)
> **작성일**: 2026-03-24

---

## 1. 목표

코어부터 서비스까지 전 레이어에 **일관된 로깅 인터페이스**와 **충분한 로그 밀도**를 확보하여,
디버깅의 1차 수단으로 로그를 활용할 수 있는 기반을 구축한다.

### 핵심 산출물
1. `ScopedLogger` 클래스 — `std::source_location` + `corr_id` trace 포함, 통일 인터페이스
2. 기존 `log_helpers.hpp` (standalone 함수) 및 `APEX_SVC_LOG_METHODS` (매크로) 폐기
3. 전 레이어(코어 + shared + 서비스) 로그 충실화 — 로그 0건 파일 해소

---

## 2. 현황 분석

### 2.1 기존 로깅 아키텍처

| 구성 요소 | 위치 | 설명 |
|-----------|------|------|
| 로거 초기화 | `apex_core/src/logging.cpp` | "apex"(프레임워크) + "app"(애플리케이션) 2개 async logger |
| standalone 헬퍼 | `apex_core/include/apex/core/log_helpers.hpp` | `apex::core::log::info(core_id, fmt, args...)` — 5개 함수 |
| ServiceBase 매크로 | `apex_core/include/apex/core/service_base.hpp` | `APEX_SVC_LOG_METHODS` — 5레벨 × 3오버로드 = 15개 메서드 |
| spdlog 패턴 | `apex_core/include/apex/core/config.hpp` | `"%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v"` — source_location 없음 |

### 2.2 문제점

1. **인터페이스 분열**: 코어(`apex::core::log::info(core_id, ...)`)와 서비스(`log_info(...)`)가 완전히 다른 호출 패턴
2. **source_location 부재**: 로그에 파일명/함수명/라인 번호 미포함 — 대량 로그에서 출처 추적 불가
3. **trace ID 부재**: MSA 구간(Gateway → Kafka → Service)에서 요청 단위 추적 불가
4. **로그 밀도 불균형**: apex_core 37문(핫패스 10개 파일 = 0건), apex_shared 71문(8개 파일 = 0건)

### 2.3 기존 자산 활용

- `std::source_location`: `APEX_ASSERT`(`assert.hpp`)에서 이미 사용 — 패턴 검증됨
- `corr_id`: Kafka `MetadataPrefix`(40바이트)에 이미 `uint64_t corr_id` 필드 존재 — 요청-응답 매핑용, trace_id로 겸용 가능
- spdlog 비동기 로깅: 이미 구성됨 (queue_size=8192, overrun_oldest)

---

## 3. ScopedLogger 설계

### 3.1 source_location 캡처 전략

C++ variadic template + trailing default parameter 조합이 불가하므로,
**암시적 변환 래퍼**를 사용하여 호출부에서 자동 캡처한다:

```cpp
/// source_location을 암시적으로 캡처하는 래퍼.
/// 호출부에서 문자열 리터럴이 첫 인자로 전달되면 log_loc가 먼저 매칭되어
/// source_location::current()가 호출 지점에서 평가된다.
struct log_loc {
    std::source_location loc;
    constexpr log_loc(std::source_location l = std::source_location::current()) : loc(l) {}
};
```

> **`constexpr` 사용** — `consteval`은 런타임 구성 시 문제를 일으킬 수 있으므로 `constexpr`로 한다.
> MSVC + GCC 양쪽에서 구현 초기에 컴파일 검증 필수.

### 3.2 클래스 정의

```cpp
// apex_core/include/apex/core/scoped_logger.hpp

#pragma once
#include <spdlog/spdlog.h>
#include <source_location>
#include <cstdint>
#include <string>

namespace apex::core
{

struct log_loc { /* 3.1 참조 */ };

class ScopedLogger
{
public:
    /// @param component 컴포넌트 이름 (예: "CoreEngine", "AuthService")
    /// @param core_id   코어 ID. 코어 컨텍스트 밖에서는 NO_CORE 사용
    /// @param logger_name spdlog 로거 이름. "apex"(프레임워크) 또는 "app"(서비스)
    static constexpr uint32_t NO_CORE = UINT32_MAX;

    ScopedLogger(std::string component, uint32_t core_id,
                 std::string_view logger_name = "apex");

    /// corr_id(trace) 바인딩된 자식 로거 반환 (값 복사, 스택 로컬로 사용)
    [[nodiscard]] ScopedLogger with_trace(uint64_t corr_id) const;

    // --- 기본 로그 (6레벨) ---
    template <typename... Args>
    void trace(log_loc loc, fmt::format_string<Args...> fmt, Args&&... args);
    template <typename... Args>
    void debug(log_loc loc, fmt::format_string<Args...> fmt, Args&&... args);
    template <typename... Args>
    void info(log_loc loc, fmt::format_string<Args...> fmt, Args&&... args);
    template <typename... Args>
    void warn(log_loc loc, fmt::format_string<Args...> fmt, Args&&... args);
    template <typename... Args>
    void error(log_loc loc, fmt::format_string<Args...> fmt, Args&&... args);
    template <typename... Args>
    void critical(log_loc loc, fmt::format_string<Args...> fmt, Args&&... args);

    // --- +세션 컨텍스트 (6레벨) ---
    template <typename... Args>
    void trace(log_loc loc, const SessionPtr& session,
               fmt::format_string<Args...> fmt, Args&&... args);
    // debug, info, warn, error, critical 동일 패턴

    // --- +세션+메시지ID 컨텍스트 (6레벨) ---
    template <typename... Args>
    void trace(log_loc loc, const SessionPtr& session, uint32_t msg_id,
               fmt::format_string<Args...> fmt, Args&&... args);
    // debug, info, warn, error, critical 동일 패턴

private:
    std::string component_;               // 값 소유 — 댕글링 위험 제거
    uint32_t core_id_{0};
    uint64_t corr_id_{0};                 // 0 = 미설정 (출력 생략)
    std::shared_ptr<spdlog::logger> logger_;  // 명명된 로거 캐시

    /// source_location + 컨텍스트를 %v 메시지에 직접 포맷하여 spdlog에 전달
    void log_impl(spdlog::level::level_enum level,
                  const std::source_location& loc,
                  std::string_view message);
};

} // namespace apex::core
```

**설계 결정:**
- **`std::string component_`**: 값 소유로 댕글링 참조 위험 제거. 컴포넌트 이름은 짧고 객체당 하나라 성능 영향 없음
- **`logger_name` 파라미터**: 프레임워크 코어는 `"apex"`, 서비스 코드는 `"app"` 로거 사용. 기존 2-로거 아키텍처 유지
- **`NO_CORE` 상수**: Kafka consumer 스레드, Redis reconnection 등 코어 컨텍스트 밖에서 사용. 출력: `[core=?]` 또는 `[core]` 태그 생략
- **critical 레벨 추가**: 기존 매크로는 5레벨이었으나, APEX_ASSERT와 크래시 경로를 위해 6레벨로 확장
- **스레드 안전**: ScopedLogger 자체는 스레드 안전하지 않음 (per-core 단일 스레드 아키텍처). 어댑터 백그라운드 스레드에서 사용 시 스레드별 별도 인스턴스 생성

### 3.3 출력 포맷 전략

**spdlog 패턴은 변경하지 않는다** — 기존 패턴 `"%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v"` 유지.

source_location 정보는 `ScopedLogger::log_impl()`이 **`%v` 메시지 문자열에 직접 포맷**한다.
이 방식이 `%s:%# %!` 패턴 플래그보다 안전한 이유:
- 마이그레이션 중 ScopedLogger를 거치지 않는 기존 spdlog 호출(server.cpp, tcp_acceptor.cpp 등)이
  빈 source_loc을 출력하는 문제를 방지
- 점진적 마이그레이션 가능 — ScopedLogger 도입 파일만 위치 정보가 추가됨

```
// 출력 예시 (spdlog 패턴은 기존과 동일, %v 부분만 확장)

// 기본 로그
2026-03-24 01:42:31.123 [info] [apex] [core_engine.cpp:142 CoreEngine::run] [core=0][CoreEngine] tick completed

// +세션
2026-03-24 01:42:31.123 [debug] [app] [auth_service.cpp:167 AuthService::on_login] [core=2][AuthService][sess=7] PG user lookup

// +세션+메시지ID+trace
2026-03-24 01:42:31.123 [debug] [app] [auth_service.cpp:197 AuthService::on_login] [core=2][AuthService][sess=7][msg=0x0012][trace=a1b2c3d4] token issued

// 코어 컨텍스트 밖 (NO_CORE)
2026-03-24 01:42:31.123 [warn] [apex] [redis_connection.cpp:45 RedisConnection::reconnect] [RedisConnection] reconnecting...
```

### 3.4 with_trace() 사용 패턴

`with_trace()`는 값 복사로 새 ScopedLogger를 반환한다 (경량 — string + 3개 스칼라 + shared_ptr).
**스택 로컬 변수로 사용**하며, 코루틴 `co_await` 시 코루틴 프레임에 자연스럽게 보존된다.

```cpp
// TCP 핸들러 (Gateway): 요청 단위
boost::asio::awaitable<void> GatewayService::on_message(SessionPtr session, ...) {
    auto traced = logger_.with_trace(router_->generate_corr_id());
    traced.info(session, "routing msg_id=0x{:04X} to {}", msg_id, topic);
    co_await send_to_kafka(envelope);  // traced는 코루틴 프레임에 보존
    traced.debug(session, "kafka produce completed");
}

// Kafka 핸들러 (Auth): Kafka 메시지 단위
boost::asio::awaitable<void> AuthService::on_login(KafkaMessageMeta meta, ...) {
    auto traced = logger_.with_trace(meta.corr_id);
    traced.debug(session, "processing login for user_id={}", user_id);
    co_await pg_->query(...);
    traced.info(session, "login success");
}
```

---

## 4. Trace ID (corr_id) 설계

### 4.1 전파 경로

```
[클라이언트] → WireHeader 12B (trace_id 없음)
     ↓
[Gateway]   → corr_id 생성 (unique ID)
     ↓
[Kafka]     → MetadataPrefix.corr_id에 실어 전송
     ↓
[서비스]     → KafkaMessageMeta.corr_id로 수신 → ScopedLogger.with_trace(corr_id)
     ↓
[응답]       → 동일 corr_id 보존하여 역방향 전파
```

### 4.2 기존 인프라 활용

| 구조체 | 위치 | corr_id 필드 |
|--------|------|-------------|
| `MetadataPrefix` | `apex_shared/.../kafka_envelope.hpp` | `uint64_t corr_id` (오프셋 6, 8바이트) |
| `KafkaMessageMeta` | `apex_core/.../kafka_message_meta.hpp` | `uint64_t corr_id` |
| `EnvelopeBuilder` | `apex_shared/.../envelope_builder.hpp` | `.metadata(core_id, corr_id, ...)` |

**와이어 프로토콜(WireHeader) 변경 없음** — Gateway가 요청 수신 시 corr_id를 생성하고,
Kafka 메시지에만 실어 보낸다. 클라이언트 구간은 Gateway 로그에서 `[sess=N][trace=corr_id]`로 연결.

### 4.3 ScopedLogger 연동

§3.4 `with_trace()` 사용 패턴 참조. Gateway에서 `router_->generate_corr_id()`로 생성,
서비스에서 `KafkaMessageMeta.corr_id`로 수신하여 ScopedLogger에 바인딩.

---

## 5. 마이그레이션 계획

### 5.1 폐기 대상

| 대상 | 파일 | 대체 |
|------|------|------|
| `apex::core::log::trace/debug/info/warn/error` | `log_helpers.hpp` | `ScopedLogger` 멤버 |
| `APEX_SVC_LOG_METHODS` 매크로 | `service_base.hpp` | `ScopedLogger` 멤버 |
| `log_info()`, `log_debug()` 등 호출 패턴 | 서비스 전체 | `logger_.info()` 패턴 |

### 5.2 구현 순서

```
① ScopedLogger 클래스 구현 + 단위 테스트 (log_loc, NO_CORE, with_trace 등)
② log_helpers.hpp → 코어 클래스에 ScopedLogger 멤버 추가 + 기존 테스트 수정
③ APEX_SVC_LOG_METHODS 삭제 → ServiceBase에 ScopedLogger 멤버 (logger_name="app")
④ 서비스 코드 log_info() → logger_.info() 마이그레이션 + 기존 테스트 수정
⑤ apex_shared 어댑터에 ScopedLogger 멤버 추가 (백그라운드 스레드: NO_CORE)
⑥ 로그 0건 파일에 레벨별 로그 심기 (코어 10개 + shared 8개 + 서비스)
⑦ CMakeLists.txt 갱신 (scoped_logger.cpp, test_scoped_logger.cpp 추가)
```

> **spdlog 패턴 변경 없음** — §3.3에서 결정한 대로 source_location은 `%v`에 수동 포맷.
> 기존 spdlog 호출은 수정 없이 동작하며, ScopedLogger 도입 파일만 위치 정보가 추가된다.

---

## 6. 로그 충실화 상세

### 6.1 로그 레벨 기준

| 레벨 | 기준 | 핫패스 사용 |
|------|------|------------|
| trace | 개별 데이터 흐름, 반복 호출 (프레임 인코딩, 큐 push/pop) | O (비활성 시 오버헤드 0) |
| debug | 상태 전이, 분기 결정 (세션 상태, 디스패치 경로) | O |
| info | 컴포넌트 생명주기 (시작/종료, 설정 로드) | X |
| warn | 비정상이지만 복구 가능 (큐 포화, 재연결, 타임아웃) | X |
| error | 실패, 데이터 손실 위험 (파싱 실패, 연결 끊김) | X |
| critical | 크래시 직전 (assert 실패, 메모리 고갈) | X |

### 6.2 apex_core 대상 (10개 파일, 현재 로그 0건)

| 파일 | 우선순위 | 로그 포인트 |
|------|----------|-------------|
| `session.cpp` | 높음 | 쓰기 큐 enqueue/drain, 상태 전이(connected→closing→closed), close 시퀀스 |
| `cross_core_dispatcher.cpp` | 높음 | 디스패치 시도/성공/실패, 대상 코어 선택 |
| `frame_codec.cpp` | 중간 | 프레임 파싱 결과, 바이트 소비량 (trace) |
| `config.cpp` | 중간 | 설정 키 파싱 성공, 기본값 폴백 (info/debug) |
| `periodic_task_scheduler.cpp` | 중간 | 태스크 등록/실행/제거 |
| `error_sender.cpp` | 중간 | 에러 응답 전송 (debug), 전송 실패 (error) |
| `arena_allocator.cpp` | 낮음 | 블록 할당/해제 (trace), 풀 고갈 (warn) |
| `slab_allocator.cpp` | 낮음 | 슬랩 할당/해제 (trace) |
| `ring_buffer.cpp` | 낮음 | push/pop (trace), 오버플로우 (warn) |
| `wire_header.cpp` | 낮음 | 파싱 실패 (debug) |

### 6.3 apex_shared 대상 (8개 파일, 현재 로그 0건)

| 파일 | 우선순위 | 로그 포인트 |
|------|----------|-------------|
| `circuit_breaker.cpp` | 높음 | 상태 전이 (closed→open→half-open), 트립 조건 |
| `pg_transaction.cpp` | 높음 | BEGIN/COMMIT/ROLLBACK, 쿼리 실행 결과 |
| `redis_connection.cpp` | 중간 | 연결/해제, 명령 실패 |
| `cancellation_token.cpp` | 중간 | 토큰 발행/취소 |
| `hiredis_asio_adapter.cpp` | 중간 | 비동기 이벤트 처리 |
| `adapter_error.cpp` | 낮음 | 에러 생성 (trace) |
| `kafka_config.cpp` | 낮음 | 설정 파싱 |
| `redis_config.cpp` | 낮음 | 설정 파싱 |

### 6.4 apex_services 대상

서비스 레이어는 이미 ~160문의 로그가 있으나, ScopedLogger 마이그레이션 시 다음을 보강:
- **Gateway**: 라우팅 결정(msg_id → 토픽), JWT 검증 결과, rate limit 트리거
- **Auth**: bcrypt 타이밍, 토큰 발급/갱신/블랙리스트 상세
- **Chat**: 방 생성/참가/퇴장 상태 전이, Kafka publish 결과

### 6.5 핫패스 성능 고려

- `spdlog::should_log()` = atomic load 1회 → 비활성 레벨 오버헤드 ≈ 0
- `std::source_location` = 컴파일 타임 상수 → 런타임 비용 없음
- 핫패스(할당기, 링버퍼, 프레임 코덱)는 trace 레벨만 사용
- 프로덕션에서 info 이상으로 설정하면 핫패스 로그 자동 스킵

---

## 7. 수정 대상 파일 요약

### 신규 생성
- `apex_core/include/apex/core/scoped_logger.hpp` — ScopedLogger + log_loc 정의
- `apex_core/src/scoped_logger.cpp` — log_impl() 구현
- `apex_core/tests/unit/test_scoped_logger.cpp` — 단위 테스트

### 주요 수정
- `apex_core/CMakeLists.txt` — scoped_logger 소스/테스트 추가
- `apex_core/src/logging.cpp` — 로거 초기화 시 ScopedLogger 호환 확인
- `apex_core/include/apex/core/service_base.hpp` — APEX_SVC_LOG_METHODS 삭제, ScopedLogger 멤버 추가 (logger_name="app")
- `apex_core/include/apex/core/log_helpers.hpp` — 폐기 (삭제)
- `apex_core/include/apex/core/core_engine.hpp` — ScopedLogger 멤버 추가 (logger_name="apex")
- `apex_core/src/session.cpp` — 로그 추가
- `apex_core/src/cross_core_dispatcher.cpp` — 로그 추가
- `apex_core/src/frame_codec.cpp` — 로그 추가
- `apex_core/src/config.cpp` — 로그 추가
- (+ 로그 0건 파일 전체)
- `apex_shared/lib/*/src/*.cpp` — ScopedLogger 도입 + 로그 추가 (백그라운드 스레드는 NO_CORE)
- `apex_services/*/src/*.cpp` — log_info() → logger_.info() 마이그레이션 + 로그 보강

> **spdlog 패턴(`config.hpp`)은 변경하지 않음** — §3.3 참조

---

## 8. 후속 작업 (이번 스코프 외)

| 백로그 | 내용 |
|--------|------|
| BACKLOG-175 | v0.6.1 Prometheus 메트릭 노출 |
| BACKLOG-176 | v0.6.2 Docker 멀티스테이지 프로덕션 빌드 |
| BACKLOG-177 | v0.6.3 K8s + Helm |
| BACKLOG-178 | v0.6.4 CI/CD 고도화 |
| BACKLOG-179 | 런타임 로그 레벨 동적 전환 |

Phase 5.5 후속 (별도 브랜치):
- 에러 타입 통일 (DispatchError → ErrorCode)
- 메모리 아키텍처 (intrusive_ptr, SlabPool)
- 핫패스 아키텍처 (drain/tick 분리, zero-copy dispatch)
