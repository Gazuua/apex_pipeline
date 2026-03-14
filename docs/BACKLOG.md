# BACKLOG

미해결 TODO/백로그 항목 집약. 새 작업 시작 전 반드시 확인.
완료된 항목은 즉시 삭제 (git에 이력 보존).

---

## Critical / High

- [ ] **Redis AUTH 미구현 + password 필드 미사용** — RedisConfig::password 존재하나 연결 시 AUTH 명령 미전송. 비동기 연결 핸드셰이크 재설계 필요 — 출처: review/20260314_085000_backlog.md, review/20260314_090000_auto-review.md (v0.5)

## Medium

- [ ] **PgPool acquire() immediate-fail 설계** — 풀 고갈 시 재시도/백프레셔 미구현, 즉시 실패 반환. v0.5에서 재시도 정책 고려 — 출처: review/20260314_140000_auto-review.md (v0.5)
- [ ] **KafkaAdapter draining_ 이중 추적 + per-core 접근 패턴 혼재** — draining_ bool이 api→logic 경계를 넘나들며 사용됨. 접근 패턴 정리 필요 — 출처: review/20260314_140000_auto-review.md (v0.5)
- [x] **RedisMultiplexer privdata FIFO 가정** — 확인 완료 — Redis 단일 연결에서 hiredis 콜백 FIFO 보장 (Redis 프로토콜 명세) — 출처: review/20260314_085000_backlog.md, review/20260314_090000_auto-review.md (v0.5)
- [ ] **RedisReply ARRAY 타입 미지원** — SMEMBERS/LRANGE 등 배열 응답 파싱 불가 — 출처: review/20260314_090000_auto-review.md (v0.5)
- [ ] **RedisMultiplexer pipeline() 순차 실행** — 현재 cmd 하나씩 전송+대기. 진짜 파이프라이닝(모든 cmd 전송 후 응답 일괄 수집)은 별도 설계 필요 — 출처: progress/20260314_190000_v0.4.5_progress.md (v0.5)
- [ ] **ConnectionHandler 단위 테스트 부재** — 다수 리뷰에서 반복 지적, E2E 간접 커버 중. 상당한 작업량 — 출처: review/20260314_140000_auto-review.md, review/20260313_005117_auto-review.md (v0.5)
- [ ] **PgTransaction begun_ 경로 unit test 불가** — begin()이 async이므로 mock PgConnection 또는 integration test 필요 — 출처: review/20260314_090000_auto-review.md (v0.5)
- [ ] **Server 라이프사이클 에러 경로 테스트** — 별도 태스크로 분리 — 출처: review/20260314_090000_auto-review.md (v0.5)
- [ ] **RedisMultiplexer 코루틴 명령 테스트** — async 테스트 인프라 필요 — 출처: review/20260314_140000_auto-review.md (v0.5)
- [ ] **Session async_recv 테스트** — 별도 태스크 — 출처: review/20260314_140000_auto-review.md (v0.5)
- [ ] **per-session write queue (concurrent write 방지)** — 채팅 브로드캐스트/push 알림 시 같은 세션에 동시 async_send 발생 가능. per-session write queue로 직렬화 필요. ConnectionHandler 분리(3.1) 시 send_message() 추상화 계층 고려 — 출처: plans/phase5_5_v6.md B-4 (v0.5)
- [ ] **CI에서 Windows apex_shared 어댑터 빌드 미검증** — CI가 apex_core만 빌드, apex_shared 어댑터는 Windows에서 미커버 — 출처: review/20260314_140000_auto-review.md (v0.5+)
- [ ] **pgbouncer md5 auth** — 로컬 개발 환경 인증 방식 개선 필요 — 출처: review/20260314_140000_auto-review.md (v0.5+)

## 어댑터 회복력 (E-2) — v0.5.2 Gateway 마일스톤

### 현재 상태: 최소 구현 (init 실패 throw)

### TODO (v0.5.2)
- [ ] 공통 retry with exponential backoff 레이어
- [ ] Circuit Breaker 패턴 (연속 실패 시 빠른 실패 반환)
- [ ] Dead Letter Queue (Kafka Consumer 처리 실패 메시지)
- [ ] Redis AUTH 구현

### 관련 설계서
- Apex_Pipeline.md §5 안정성 설계

## Low / Info

- [ ] **design-decisions ADR-04 초기 디렉토리 구조 stale** — 모노레포 전환 이후 ADR-04 내용이 현재 구조와 불일치 — 출처: review/20260314_140000_auto-review.md
- [ ] **Apex_Pipeline.md apex_shared/schemas/ placeholder** — 설계 문서에 schemas/ 디렉토리 언급되나 아직 미생성 — 출처: review/20260314_140000_auto-review.md
- [ ] **테스트 이름 오타 MoveConstruction 2건** — test_bump_allocator.cpp:103, test_arena_allocator.cpp:109에서 `MoveConstruction` (오타 여부 확인 필요) — 출처: review/20260314_140000_auto-review.md
- [ ] **make_socket_pair 반환 순서 불일치** — 함수는 `{server, client}` 반환하나 일부 호출처에서 순서 혼용 — 출처: review/20260314_140000_auto-review.md
- [ ] **plans-progress 추적성 갭 2건** — docs-consolidation, roadmap-redesign 계획서에 대응하는 progress 문서 없음 (초기 레거시) — 출처: review/20260314_140000_auto-review.md
- [ ] **vcpkg.json 의존성 정리** — 빌드 영향 큼, 별도 태스크 — 출처: review/20260314_140000_auto-review.md
- [ ] **BumpAllocator / ArenaAllocator 벤치마크** — malloc 대비 성능 기준 수치 확보 필요 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.5)
- [ ] **NUMA 바인딩 + Core Affinity** — 멀티 소켓 환경 전용, 싱글 소켓에서는 불필요 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.6+)
- [ ] **mmap 직접 사용 (malloc 대체)** — RSS 모니터링/메모리 프로파일링 도입 시 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.6+)
- [ ] **Hugepage (대형 페이지)** — TLB miss 병목 확인 후 적용 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.6+)
- [ ] **코루틴 프레임 풀 할당 (ADR-21)** — 벤치마크에서 코루틴 프레임 힙 할당이 병목으로 확인될 경우 커스텀 코루틴 타입(promise_type 풀 오버로드) 도입 검토. 현재는 mimalloc/jemalloc + HALO 최적화로 대응 — 출처: plans/roadmap-redesign.md, design-decisions.md (벤치마크 후)
- [ ] **L1 로컬 캐시** — per-core 핫 데이터 캐싱. 부하 테스트에서 캐시 미스가 병목으로 확인 시 도입 — 출처: plans/roadmap-redesign.md (v1.0 부하 테스트 후)
- [ ] **auto-review 리뷰어 확장 (v0.5+)** — reviewer-protocol (서비스 간 FlatBuffers/메시지 스키마), reviewer-adapter (Kafka/Redis/PG 사용 패턴), reviewer-perf (핫패스 분석/벤치마크 회귀) 3명 추가 예정 — 출처: plans/auto-review-redesign.md
- [ ] **README 빌드 안내 보강** — 별도 작업 — 출처: review/20260314_140000_auto-review.md
