# BACKLOG-52: 디버깅/운영 흐름 로깅 대폭 추가 — 설계 스펙

## 배경

- apex_core 핫패스 10개 소스에 debug/trace 로그 0건
- 전체 spdlog 호출 ~180건 중 debug 10건, trace 0건
- 운영 서버에서 문제 발생 시 에러 로그만 확인 가능, 흐름 추적 불가
- core_id/session_id가 체계적으로 포함되지 않아 멀티코어 로그 분석 어려움

## 설계 결정

### 채택: ServiceBase 로깅 헬퍼 (Approach A)

- ServiceBase에 protected 헬퍼 메서드 제공, core_id + service_name 자동 주입
- 3가지 오버로드: 기본 / 세션 컨텍스트 / 핸들러 컨텍스트
- 비ServiceBase 코드용 standalone 헬퍼 (`apex/core/log_helpers.hpp`)

### 탈락: Named Logger (B안 A+C 조합)

- MSA 프로세스 분리로 per-service 레벨 제어가 이미 가능
- 헬퍼 내부 로거 참조만 변경하면 나중에 전환 가능 (재작업 비용 ~0)

### 탈락: MDC/TLS 패턴 (B안)

- spdlog 커스텀 포매터 구현 필요, 복잡도 대비 이득 부족
- 헬퍼가 동일한 컨텍스트 자동 주입을 달성

## API 설계

### ServiceBase 헬퍼 (service_base.hpp)

```cpp
// 각 레벨(trace/debug/info/warn/error) × 3 오버로드 = 15개

// 1) 기본 — [core=N][서비스명]
log_debug("pool exhausted, fallback to malloc");
// → [core=2][chat] pool exhausted, fallback to malloc

// 2) 세션 — [sess=N] 추가
log_debug(session, "timeout, closing");
// → [core=2][chat][sess=42] timeout, closing

// 3) 핸들러 — [msg=0xNNNN] 추가
log_debug(session, msg_id, "handler entered, size={}", payload.size());
// → [core=2][chat][sess=42][msg=0x1000] handler entered, size=128
```

구현 핵심:
```cpp
template<typename... Args>
void log_debug(fmt::format_string<Args...> fmt, Args&&... args) {
    if (spdlog::should_log(spdlog::level::debug)) {
        spdlog::debug("[core={}][{}] {}",
            per_core_->core_id, name_,
            fmt::format(fmt, std::forward<Args>(args)...));
    }
}
```

### Standalone 헬퍼 (apex/core/log_helpers.hpp, 신규)

```cpp
namespace apex::core::log {
    // CoreEngine, ConnectionHandler 등 프레임워크 내부용
    debug(uint32_t core_id, fmt, args...);  // → [core=2] message
}
```

## 로그 배치 계획

### ① 세션 수명 — session_manager.cpp, connection_handler

| 레벨 | 내용 |
|------|------|
| info | 세션 생성, 세션 종료(사유) |
| debug | refcount 변화, close 경로(정상/비정상/타임아웃) |

### ② 핸들러 디스패치 — message_dispatcher, 서비스 핸들러

| 레벨 | 내용 |
|------|------|
| debug | 핸들러 진입, 완료 + 소요시간 |
| warn | 처리 시간 임계치 초과, 미등록 msg_id |

### ③ 크로스코어 SPSC — core_engine.cpp, spsc_mesh.cpp

| 레벨 | 내용 |
|------|------|
| debug | post_to 발생(대상 코어, 메시지 타입) |
| trace | enqueue/dequeue 상세(큐 사용량) |
| warn | 큐 full (기존 유지) |

### ④ 어댑터 I/O — apex_shared/ Redis·PG·Kafka

| 레벨 | 내용 |
|------|------|
| debug | 호출 전후(명령, 소요시간) |
| warn | 재연결, 타임아웃, 풀 고갈 |

### ⑤ 코루틴 수명 — service_base spawn(), core_engine spawn_tracked()

| 레벨 | 내용 |
|------|------|
| debug | 코루틴 시작(이름), 완료 |
| error | 예외 탈출 (기존 유지 + 강화) |

### 서비스 레이어 보강

Gateway pipeline, Auth/Chat 핸들러 주요 분기점에 debug 추가.
기존 159건 있으므로 빈 곳만 보강.

## 스코프 외

- Named logger 전환 (MSA 프로세스 분리로 불필요)
- MDC trace_id (헬퍼로 대체)
- 런타임 로그 레벨 전환 API (별도 백로그)
- 로그 보존 정책 (BACKLOG-61)

## 변경 대상 파일 (예상)

**신규**: `apex_core/include/apex/core/log_helpers.hpp`

**수정 (헬퍼)**: `apex_core/include/apex/core/service_base.hpp`

**수정 (로그 추가)**:
- apex_core: `core_engine.cpp`, `session_manager.cpp`, `spsc_mesh.cpp`, `server.cpp`, `service_base.hpp`(spawn)
- apex_shared: Redis/PG/Kafka 어댑터, `connection_handler` 관련
- apex_services: gateway_pipeline, auth/chat 핸들러 보강
