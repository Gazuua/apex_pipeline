# 테스트/벤치마크 보강 — BACKLOG #142, #171, #200

**날짜**: 2026-03-26
**브랜치**: (착수 시 생성)
**스코프**: CORE, GATEWAY

## 대상 백로그

| ID | 제목 | 유형 |
|----|------|------|
| #142 | CrashHandler 단위 테스트 부재 | TEST |
| #171 | Gateway 내부 컴포넌트 단위 테스트 추가 | TEST |
| #200 | intrusive_ptr vs shared_ptr 멀티스레드 경합 벤치마크 | PERF |

## #142 CrashHandler 테스트 설계

### 접근법

GTest `EXPECT_DEATH` 사용. 기존 `SpscMeshDeathTest` 패턴을 따름.
별도 subprocess 인프라 구축 불필요 — GTest가 내부적으로 처리 (Unix: fork, Windows: CreateProcess).

### 테스트 케이스

| # | 시나리오 | 방법 | 검증 |
|---|---------|------|------|
| A | install → SIGABRT → crash 메시지 출력 + 종료 | `EXPECT_DEATH` | stderr에 `\[APEX CRASH\] Signal: SIGABRT` 매칭 |
| B | install → uninstall → SIGABRT → crash 메시지 없음 | `EXPECT_DEATH` | stderr에 `APEX CRASH` 미포함 확인 |
| C | Sanitizer 빌드 → 전체 스킵 | `#ifdef` 조건부 `GTEST_SKIP()` | 컴파일타임 결정 |
| D | Windows SEH → null deref → exception hex 출력 | `EXPECT_DEATH` + `#ifdef _WIN32` | stderr에 `Unhandled Windows exception: 0x` 매칭 |

### 파일

- `apex_core/tests/unit/test_crash_handler.cpp` (신규)

### 제약

- Death test fixture에 `GTEST_FLAG_SET(death_test_style, "threadsafe")` 필수
- Release 빌드 조건부 로직 불필요 — crash handler는 Debug/Release 모두 동작
- Sanitizer 빌드에서는 `is_sanitizer_active()` → install이 no-op이므로 전체 스킵
- 테스트 B(uninstall 검증)에서 SIGABRT는 프로세스를 종료시키므로 EXPECT_DEATH 내부에서 수행

## #171 Gateway 컴포넌트 단위 테스트 추가

### 현재 커버리지 분석

기존 Gateway 테스트 파일을 확인하여 미커버 컴포넌트를 식별하고 테스트를 추가한다.
기존 패턴(io_context 로컬 생성, 포트 0 사용, Fixture SetUp/TearDown)을 따름.

### 파일

- 기존 `apex_services/gateway/tests/test_*.cpp` 보강 또는 신규 파일

## #200 intrusive_ptr vs shared_ptr 벤치마크

### 접근법

기존 벤치마크 패턴(`bench_main.cpp` + Google Benchmark)을 따름.
멀티스레드 경합 시나리오에서 intrusive_ptr과 shared_ptr의 refcount 연산 성능을 비교.

### 벤치마크 시나리오

- Single-thread refcount increment/decrement throughput
- Multi-thread contention (N threads, shared object refcount 경합)
- Session-like lifecycle (create → share across threads → release)

### 파일

- `apex_core/benchmarks/micro/bench_intrusive_vs_shared.cpp` (신규)
