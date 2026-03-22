# TSAN Flaky Test 수정 — PostToFullSpscQueueReturnsFalse

- **브랜치**: `bugfix/tsan-core-engine-flaky-test`
- **PR**: #112
- **상태**: 완료 (CI 전체 통과)

## 문제

`CoreEngineTest.PostToFullSpscQueueReturnsFalse` 테스트가 linux-tsan에서 간헐 실패.
`post_to()`가 매번 `schedule_drain()`을 호출하여 core 1에 drain task를 스케줄링하는데,
TSAN 오버헤드로 인해 post 사이에 core 1이 drain을 완료하여 큐가 가득 차지 않음.

## 수정

Core 1의 io_context에 mutex/cv blocker를 먼저 post하여 drain 실행을 차단.
SPSC 큐가 결정적(deterministic)으로 가득 차도록 수정.

## 변경 파일

- `apex_core/tests/unit/test_core_engine.cpp` — 테스트 1건 수정 (+32 -2)
