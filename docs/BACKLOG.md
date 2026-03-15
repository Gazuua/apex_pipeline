# BACKLOG

미해결 TODO/백로그 항목 집약. 새 작업 시작 전 반드시 확인.
완료된 항목은 즉시 삭제 (git에 이력 보존).

---

## Critical / High

### [Critical] v0.5 기능 개발 본격 진행
- **위치**: 프로젝트 전체
- **상태**: 미구현
- **배치**: Wave 1 전체 선행
- **설명**: 개발 기반환경(빌드, clangd LSP, auto-review, CI) 세팅 완료. 환경 작업 최소화하고 기능 구현에 집중 — v0.4.5.2 기준

### [High] Redis AUTH 미구현 + password 필드 미사용
- **위치**: `apex_shared/` RedisConfig / 비동기 연결 핸드셰이크
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: RedisConfig::password 존재하나 연결 시 AUTH 명령 미전송. 비동기 연결 핸드셰이크 재설계 필요 — 출처: review/20260314_085000_backlog.md, review/20260314_090000_auto-review.md (v0.5)

---

## 어댑터 회복력

### [Medium] PgPool acquire() immediate-fail 설계
- **위치**: `apex_shared/` PgPool
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: 풀 고갈 시 재시도/백프레셔 미구현, 즉시 실패 반환. v0.5에서 재시도 정책 고려 — 출처: review/20260314_140000_auto-review.md (v0.5)

### [Medium] KafkaAdapter draining_ 이중 추적 + per-core 접근 패턴 혼재
- **위치**: `apex_shared/` KafkaAdapter
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: draining_ bool이 api→logic 경계를 넘나들며 사용됨. 접근 패턴 정리 필요 — 출처: review/20260314_140000_auto-review.md (v0.5)

### [Medium] RedisReply ARRAY 타입 미지원
- **위치**: `apex_shared/` RedisReply
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: SMEMBERS/LRANGE 등 배열 응답 파싱 불가 — 출처: review/20260314_090000_auto-review.md (v0.5)

### [Medium] RedisMultiplexer pipeline() 순차 실행
- **위치**: `apex_shared/` RedisMultiplexer
- **상태**: 부분 구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: 현재 cmd 하나씩 전송+대기. 진짜 파이프라이닝(모든 cmd 전송 후 응답 일괄 수집)은 별도 설계 필요 — 출처: progress/20260314_190000_v0.4.5_progress.md (v0.5)

### [Medium] 공통 retry with exponential backoff 레이어
- **위치**: `apex_shared/` 어댑터 공통
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: 어댑터 전반에 공통 재시도 레이어 필요 — 출처: 어댑터 회복력 (E-2) v0.5.2 Gateway 마일스톤

### [Medium] Circuit Breaker 패턴
- **위치**: `apex_shared/` 어댑터 공통
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: 연속 실패 시 빠른 실패 반환 — 출처: 어댑터 회복력 (E-2) v0.5.2 Gateway 마일스톤

### [Medium] Dead Letter Queue (Kafka Consumer)
- **위치**: `apex_shared/` KafkaAdapter
- **상태**: 미구현
- **배치**: Wave 1 — B. 어댑터 회복력
- **설명**: Kafka Consumer 처리 실패 메시지를 별도 큐로 격리 — 출처: 어댑터 회복력 (E-2) v0.5.2 Gateway 마일스톤

### 관련 설계서
- Apex_Pipeline.md §5 안정성 설계

---

## 프로토콜 + 스키마

### [Medium] per-session write queue (concurrent write 방지)
- **위치**: `apex_core/` ConnectionHandler / Session
- **상태**: 미구현
- **배치**: Wave 1 — C. 프로토콜 + 스키마
- **설명**: 채팅 브로드캐스트/push 알림 시 같은 세션에 동시 async_send 발생 가능. per-session write queue로 직렬화 필요. ConnectionHandler 분리(3.1) 시 send_message() 추상화 계층 고려 — 출처: plans/phase5_5_v6.md B-4 (v0.5)

### [Low] Apex_Pipeline.md apex_shared/schemas/ placeholder
- **위치**: `docs/Apex_Pipeline.md`, `apex_shared/schemas/`
- **상태**: 미구현
- **배치**: Wave 1 — C. 프로토콜 + 스키마
- **설명**: 설계 문서에 schemas/ 디렉토리 언급되나 아직 미생성 — 출처: review/20260314_140000_auto-review.md

---

## 코드 위생

### [Low] clang-tidy 워닝 정리 (apex_core)
- **위치**: `apex_core/` — `server.cpp`, `session.cpp`, `arena_allocator.cpp`
- **상태**: 미구현
- **배치**: Wave 1 — A. 코드 위생
- **설명**: `server.cpp` 2건 (trivially-copyable move, unnecessary-value-param), `session.cpp` 5건 (implicit-bool-conversion, owning-memory, coroutine-ref-param, unused-return-value), `arena_allocator.cpp` 8건 (prefer-member-initializer, no-int-to-ptr, owning-memory, no-malloc, implicit-bool-conversion). 기능 이상 없음, 코어 가이드라인 스타일 수준 — 출처: clangd LSP 진단 (v0.4.5.2)

### [Low] 테스트 이름 오타 MoveConstruction 2건
- **위치**: `apex_core/` — `test_bump_allocator.cpp:103`, `test_arena_allocator.cpp:109`
- **상태**: 미구현
- **배치**: Wave 1 — A. 코드 위생
- **설명**: `MoveConstruction` (오타 여부 확인 필요) — 출처: review/20260314_140000_auto-review.md

### [Low] make_socket_pair 반환 순서 불일치
- **위치**: `apex_core/` make_socket_pair 및 호출처
- **상태**: 미구현
- **배치**: Wave 1 — A. 코드 위생
- **설명**: 함수는 `{server, client}` 반환하나 일부 호출처에서 순서 혼용 — 출처: review/20260314_140000_auto-review.md

### [Low] design-decisions ADR-04 초기 디렉토리 구조 stale
- **위치**: `docs/` design-decisions ADR-04
- **상태**: 부분 구현
- **배치**: Wave 1 — A. 코드 위생
- **설명**: 모노레포 전환 이후 ADR-04 내용이 현재 구조와 불일치 — 출처: review/20260314_140000_auto-review.md

### [Low] main 히스토리 문서 전용 커밋 squash
- **위치**: main 브랜치 git 히스토리
- **상태**: 미구현
- **배치**: Wave 1 — A. 코드 위생
- **설명**: docs 전용 커밋이 전체의 30~40% 차지. interactive rebase로 인접 문서 커밋 squash 정리 필요 — 출처: 코드 리뷰 피드백

### [Low] plans-progress 추적성 갭 2건
- **위치**: `docs/` plans, progress
- **상태**: 미구현
- **배치**: Wave 1 — A. 코드 위생
- **설명**: docs-consolidation, roadmap-redesign 계획서에 대응하는 progress 문서 없음 (초기 레거시) — 출처: review/20260314_140000_auto-review.md

---

## 단위 테스트

### [Medium] ConnectionHandler 단위 테스트 부재
- **위치**: `apex_core/` ConnectionHandler
- **상태**: 미구현
- **배치**: Wave 1 — E-1. 기존 코드 단위 테스트
- **설명**: 다수 리뷰에서 반복 지적, E2E 간접 커버 중. 상당한 작업량 — 출처: review/20260314_140000_auto-review.md, review/20260313_005117_auto-review.md (v0.5)

### [Medium] PgTransaction begun_ 경로 unit test 불가
- **위치**: `apex_shared/` PgTransaction
- **상태**: 미구현
- **배치**: Wave 1 — E-1. 기존 코드 단위 테스트
- **설명**: begin()이 async이므로 mock PgConnection 또는 integration test 필요 — 출처: review/20260314_090000_auto-review.md (v0.5)

### [Medium] Server 라이프사이클 에러 경로 테스트
- **위치**: `apex_core/` Server
- **상태**: 미구현
- **배치**: Wave 1 — E-1. 기존 코드 단위 테스트
- **설명**: 별도 태스크로 분리 — 출처: review/20260314_090000_auto-review.md (v0.5)

### [Medium] RedisMultiplexer 코루틴 명령 테스트
- **위치**: `apex_shared/` RedisMultiplexer
- **상태**: 미구현
- **배치**: Wave 1 — E-1. 기존 코드 단위 테스트
- **설명**: async 테스트 인프라 필요 — 출처: review/20260314_140000_auto-review.md (v0.5)

### [Medium] Session async_recv 테스트
- **위치**: `apex_core/` Session
- **상태**: 미구현
- **배치**: Wave 1 — E-1. 기존 코드 단위 테스트
- **설명**: 별도 태스크 — 출처: review/20260314_140000_auto-review.md (v0.5)

---

## 서비스 체인

### [Medium] pgbouncer md5 auth
- **위치**: `apex_infra/` pgbouncer 설정
- **상태**: 미구현
- **배치**: Wave 2 — D. 서비스 체인
- **설명**: 로컬 개발 환경 인증 방식 개선 필요 — 출처: review/20260314_140000_auto-review.md (v0.5+)

---

## 운영 인프라

### [Medium] CI에서 Windows apex_shared 어댑터 빌드 미검증
- **위치**: `.github/` CI 워크플로우
- **상태**: 미구현
- **배치**: Wave 3 — F. 운영 인프라
- **설명**: CI가 apex_core만 빌드, apex_shared 어댑터는 Windows에서 미커버 — 출처: review/20260314_140000_auto-review.md (v0.5)

---

## 빌드 / 의존성

### [Low] vcpkg.json 의존성 정리
- **위치**: `vcpkg.json`
- **상태**: 미구현
- **배치**: Wave 배정 보류 (빌드 영향 큼, 별도 판단)
- **설명**: 빌드 영향 큼, 별도 태스크 — 출처: review/20260314_140000_auto-review.md

---

## 성능 / 최적화

### [Low] BumpAllocator / ArenaAllocator 벤치마크
- **위치**: `apex_core/` BumpAllocator, ArenaAllocator
- **상태**: 미구현
- **배치**: v0.6 전 (Wave 배정 보류)
- **설명**: malloc 대비 성능 기준 수치 확보 필요 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.5)

### [Low] 코루틴 프레임 풀 할당 (ADR-21)
- **위치**: `apex_core/` 코루틴 promise_type
- **상태**: 설계만 완료
- **배치**: v1.0 이후 (Wave 배정 보류)
- **설명**: 벤치마크에서 코루틴 프레임 힙 할당이 병목으로 확인될 경우 커스텀 코루틴 타입(promise_type 풀 오버로드) 도입 검토. 현재는 mimalloc/jemalloc + HALO 최적화로 대응 — 출처: plans/roadmap-redesign.md, design-decisions.md (벤치마크 후)

### [Low] NUMA 바인딩 + Core Affinity
- **위치**: `apex_core/` 스레드 관리
- **상태**: 미구현
- **배치**: v1.0 이후 (Wave 배정 보류)
- **설명**: 멀티 소켓 환경 전용, 싱글 소켓에서는 불필요 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.6+)

### [Low] mmap 직접 사용 (malloc 대체)
- **위치**: `apex_core/` 메모리 관리
- **상태**: 미구현
- **배치**: v1.0 이후 (Wave 배정 보류)
- **설명**: RSS 모니터링/메모리 프로파일링 도입 시 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.6+)

### [Low] Hugepage (대형 페이지)
- **위치**: `apex_core/` 메모리 관리
- **상태**: 미구현
- **배치**: v1.0 이후 (Wave 배정 보류)
- **설명**: TLB miss 병목 확인 후 적용 — 출처: docs/apex_core/backlog_memory_os_level.md (v0.6+)

### [Low] L1 로컬 캐시
- **위치**: `apex_core/` per-core 데이터 관리
- **상태**: 미구현
- **배치**: v1.0 이후 (Wave 배정 보류)
- **설명**: per-core 핫 데이터 캐싱. 부하 테스트에서 캐시 미스가 병목으로 확인 시 도입 — 출처: plans/roadmap-redesign.md (v1.0 부하 테스트 후)

---

## 도구 / 자동화

### [Low] auto-review 리뷰어 확장 (v0.5+)
- **위치**: `apex_tools/auto-review/`
- **상태**: 미구현
- **배치**: v0.5+ (Wave 배정 보류)
- **설명**: reviewer-protocol (서비스 간 FlatBuffers/메시지 스키마), reviewer-adapter (Kafka/Redis/PG 사용 패턴), reviewer-perf (핫패스 분석/벤치마크 회귀) 3명 추가 예정 — 출처: plans/auto-review-redesign.md

### [Low] README 빌드 안내 보강
- **위치**: `README.md`
- **상태**: 미구현
- **배치**: Wave 배정 보류
- **설명**: 별도 작업 — 출처: review/20260314_140000_auto-review.md
