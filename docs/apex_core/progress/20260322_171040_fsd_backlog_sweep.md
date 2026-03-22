# FSD 백로그 소탕 — #112, #20, #21, #22

**날짜**: 2026-03-22
**브랜치**: feature/fsd-backlog-20260322_163816

## 해결 항목

### #22. async_send_raw + write_pump 동시 write 위험 (MAJOR)

async_send/async_send_raw가 소켓에 직접 async_write를 호출하여 write_pump와 동시 실행 시 UB였던 문제 해결.

- `WriteRequest`에 `completion_timer` (steady_timer) + `completion_result` 필드 추가
- `enqueue_and_await()` private 메서드: timer를 time_point::max()로 생성 → 큐 적재 → pump 기동 → async_wait로 대기
- write_pump가 completion_timer 있는 항목 처리 후 cancel()로 시그널
- async_send도 동일 패턴 적용 (WireHeader 직렬화 후 enqueue)
- 테스트 6건 추가 (TC6-TC11): write_pump 경유 검증, FIFO 순서, closed session, 혼합 호출

### #21. Server multi-listener dispatcher sync_all_handlers (MAJOR)

개별 msg_id 핸들러가 primary listener에만 등록되고 보조 리스너에 전파되지 않던 문제 해결.

- `MessageDispatcher::handlers()` const 접근자 추가
- `ListenerBase::sync_all_handlers()` pure virtual + `Listener<P,T>` 구현
- server.cpp Phase 3.5에서 sync_default_handler → sync_all_handlers 교체
- 테스트 4건 추가

### #112. lock-free SessionMap 아키텍처 벤치마크 (MAJOR)

bench_architecture_comparison.cpp에 `BM_Shared_LockFree_Stateful` 변형 추가.

- `boost::concurrent_flat_map<uint64_t, SessionState>` 사용
- visit() API로 in-place 수정, cvisit_all()로 결과 집계
- Per-core vs sharded_mutex vs concurrent_flat_map 3자 비교 가능

### #20. BumpAllocator / ArenaAllocator 벤치마크 (MINOR)

bench_allocators.cpp에 실서비스 시뮬레이션 시나리오 + capacity 파라미터 스윕 추가.

- BM_BumpAllocator_RequestCycle: 3~8회 가변(32~512B) 할당 → reset
- BM_ArenaAllocator_TransactionCycle: 4~12회 가변(128~2048B) 할당 → reset
- 기존 벤치마크에 capacity/block_size 2번째 파라미터 추가

## 빌드 결과

- MSVC debug: 성공, 83 테스트 전부 통과
