# BACKLOG-132: RedisAdapter close UAF 방어 — 완료 기록

**버전**: v0.5.10.6
**PR**: #107
**브랜치**: `feature/backlog-132-redis-adapter-close-uaf`

---

## 작업 요약

RedisAdapter::do_close()의 "우연한 안전"을 "설계된 안전"으로 격상. 3번의 방향 전환(A안 awaitable, B안 재배치, 최종안 cancellation 인프라) 후 완성.

## 변경 내용

### 1. CancellationToken per-core 프리미티브
- per-core 어댑터 코루틴의 추적/취소를 담당하는 새 클래스
- `new_slot()`, `cancel_all()`, `on_complete()`, `outstanding()` 4개 API
- debug 빌드에서 스레드 소유권 assert 강제
- move constructor/assignment 지원 (vector 저장용)

### 2. AdapterBase 범용 인프라
- `spawn_adapter_coro()`: per-core 코루틴 spawn + cancellation slot 바인딩
- `cancel_all_coros()`: drain 시 자동 호출, 각 코어에 post
- `outstanding_adapter_coros()`: 폴링 루프에서 어댑터 카운터 합산
- `close()` 2단계: per-core `do_close_per_core()` post → 전역 `do_close()`
- DRAINING/CLOSED 상태에서 spawn 거부

### 3. RedisMultiplexer 리팩토링
- `reconnecting_` 플래그 제거 → `cancellation_signal` 기반 종료
- `close()` awaitable → 동기
- `on_disconnect()` 재진입 방어 (`reconnect_active_` + ReconnectGuard RAII)
- 생성자에 `SpawnCallback` 추가 (AdapterBase 연동)
- 소멸자: 방어적 close() + 직접 slab destroy

### 4. Server shutdown 재배치
- Step 5↔6 교환: adapter close → CoreEngine stop
- Step 4.5 폴링에 어댑터 코루틴 카운터 추가
- 각 단계에 invariant 주석

### 5. PgAdapter 마이그레이션
- `engine_` 중복 제거 → AdapterBase `base_engine_` 사용

## Auto-Review 결과
- HIGH 4건 발견 → 전부 수정 (remaining UAF, backoff_timer 누락, 소멸자 slab UAF, reconnect_active 잔류)
- LOW 3건 → 수용 가능, 미수정

## 테스트
- CancellationToken 단위 테스트 4개 추가
- 기존 84개 테스트 전수 PASS
- ASAN/TSAN CI 검증 대기 중
