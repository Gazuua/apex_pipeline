# BACKLOG-120: CircuitBreaker HALF_OPEN 성공 카운트 추적

## 결과 요약

`half_open_successes_` 카운터를 분리 도입하여 "허용된 호출 수"(`half_open_calls_`)와 "성공 횟수"의 의미를 명확히 구분. CLOSED 전이 조건을 성공 카운터 기반으로 변경.

## 변경 내역

### 헤더 (`circuit_breaker.hpp`)
- `half_open_successes_` 멤버 추가 (uint32_t, init 0)
- `half_open_successes()` const accessor 추가
- 기존 멤버에 역할 주석 추가

### 구현 (`circuit_breaker.cpp`)
- `on_success()` HALF_OPEN: `half_open_successes_` 증가, CLOSED 전이 시 리셋
- `should_allow()` OPEN→HALF_OPEN: `half_open_successes_` 초기화
- `reset()`: `half_open_successes_` 초기화 추가

### 테스트 (`test_circuit_breaker.cpp`)
- TC5: `half_open_successes()` 검증 추가
- TC9 신규: HALF_OPEN 성공 후 실패 시 성공 카운트 리셋 + 재진입 시 fresh start 검증

## 빌드 검증

- MSVC debug: 빌드 성공, 전체 테스트 통과
