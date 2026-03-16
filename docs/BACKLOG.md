# BACKLOG

미해결 TODO/백로그 항목 집약. 새 작업 시작 전 반드시 확인.
완료된 항목은 즉시 삭제 (git에 이력 보존).

---

## Critical

### [Critical] C-1. PerIpRateLimiter TTL 콜백 미연결
- **위치**: `apex_shared/lib/rate_limit/src/per_ip_rate_limiter.cpp`
- **상태**: 미구현
- **배치**: v0.5 Wave 2 패치 (머지 후 즉시)
- **설명**: TimingWheel on_expire → remove_entry 연결 부재. 만료 엔트리 메모리 잔존. core/shared 경계 설계 결정 필요 (TimingWheel은 core, PerIpRateLimiter는 shared) — 출처: auto-review (reviewer-memory)

### [Critical] C-2. ResponseDispatcher::on_response() 데이터 레이스
- **위치**: `apex_services/gateway/src/response_dispatcher.cpp`
- **상태**: 미구현
- **배치**: v0.5 Wave 2 패치 (머지 후 즉시)
- **설명**: Kafka 스레드에서 per-core PendingRequestsMap 직접 접근. cross_core_post 위임 설계 필요. 보안+정합성 영향 (다른 세션에 응답 전달 가능) — 출처: auto-review (reviewer-concurrency + reviewer-cross-cutting)

### [Critical] C-3. RefreshTokenRequest(msg_id=10) 라우팅 불가
- **위치**: `apex_services/gateway/gateway.toml`
- **상태**: 미구현
- **배치**: v0.5 Wave 2 패치
- **설명**: gateway.toml routes에 시스템 범위([0,999]) 누락. 시스템 메시지별 라우팅 분기 설계 필요 — 출처: auto-review (reviewer-cross-cutting)

---

## Important

### [Important] I-1. ChannelSessionMap 무제한 메모리 증가
- **위치**: `apex_services/gateway/include/apex/gateway/channel_session_map.hpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: per-session 구독 제한 정책 미설정. 비즈니스 결정 필요 — 출처: auto-review (reviewer-memory)

### [Important] I-2. GatewayPipeline::set_rate_limiter() 동시성 보호 없음
- **위치**: `apex_services/gateway/include/apex/gateway/gateway_pipeline.hpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: atomic 또는 strand 보장 필요. ConfigReloader 활성화 시 실제 레이스 — 출처: auto-review (reviewer-concurrency + reviewer-cross-cutting)

### [Important] I-3. review 문서 2개 상세 내용 부재
- **위치**: `docs/apex_common/review/20260315_210204_v0.5-wave1-phase*.md`
- **상태**: 미구현
- **배치**: 별도 판단 (원본 데이터 없이 복원 불가)
- **설명**: 원본 데이터 없이 복원 불가 — 출처: auto-review (reviewer-docs-records)

### [Important] I-4. FileWatcher 테스트 flaky 위험
- **위치**: `apex_services/gateway/tests/test_file_watcher.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: sleep 기반, CI 환경 의존. clock injection 향후 개선 — 출처: auto-review (reviewer-test-quality)

### [Important] I-5. PendingRequests 테스트 flaky 위험
- **위치**: `apex_services/gateway/tests/test_pending_requests.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: sleep 기반, 시간 injection 미지원 — 출처: auto-review (reviewer-test-quality)

### [Important] I-6. gateway.toml JWT secret 하드코딩
- **위치**: `apex_services/gateway/gateway.toml`
- **상태**: 미구현
- **배치**: v0.6 (운영 인프라)
- **설명**: 환경변수 치환 메커니즘 필요 — 출처: auto-review (reviewer-security)

### [Important] I-7. SQL 마이그레이션 기본 비밀번호
- **위치**: `apex_services/auth-svc/migrations/`
- **상태**: 미구현
- **배치**: v0.6 (운영 인프라)
- **설명**: 인프라 시크릿 주입 전략 필요 — 출처: auto-review (reviewer-security)

### [Important] I-8. session_store Redis 명령 인젝션
- **위치**: `apex_services/auth-svc/src/session_store.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: RedisMultiplexer printf-style API 변경 필요 — 출처: auto-review (reviewer-security)

### [Important] I-9. PubSub→클라이언트 프레임 포맷 불일치
- **위치**: `apex_services/gateway/src/broadcast_fanout.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: build_pubsub_payload vs WireHeader 계약 위반. placeholder이지만 구현 시 수정 필요 — 출처: auto-review (reviewer-cross-cutting)

### [Important] I-10. redis.ratelimit TOML 섹션 파싱 누락
- **위치**: `apex_services/gateway/src/gateway_config_parser.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: GatewayConfig + parser 변경 필요 — 출처: auto-review (reviewer-cross-cutting)

### [Important] I-11. RateLimitEndpointConfig → EndpointRateConfig 변환 경로 부재
- **위치**: `apex_services/gateway/src/config_reloader.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: hot-reload 경로 단절 — 출처: auto-review (reviewer-cross-cutting)

---

## Minor

### [Minor] m-1. user_id 파라미터 미사용
- **위치**: `apex_services/gateway/src/message_router.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: MessageRouter + MetadataPrefix에 user_id 전달 경로 없음. 설계-구현 갭 — 출처: auto-review (reviewer-cross-cutting)

---

## 코드 위생

### [Low] session.cpp clang-tidy 워닝 잔여분
- **위치**: `apex_core/` — `session.cpp`
- **상태**: 부분 구현
- **배치**: v0.5 패치
- **설명**: server.cpp(1건), arena_allocator.cpp(8건) 수정 완료. session.cpp 5건(implicit-bool-conversion, owning-memory, coroutine-ref-param, unused-return-value 등) 중 2건만 수정, 나머지 미완료 — 출처: clangd LSP 진단 (v0.4.5.2)

### [Low] 테스트 이름 오타 MoveConstruction 2건
- **위치**: `apex_core/` — `test_bump_allocator.cpp:103`, `test_arena_allocator.cpp:109`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: `MoveConstruction` (오타 여부 확인 필요) — 출처: review/20260314_140000_auto-review.md

### [Low] make_socket_pair 반환 순서 불일치
- **위치**: `apex_core/` make_socket_pair 및 호출처
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: 함수는 `{server, client}` 반환하나 일부 호출처에서 순서 혼용 — 출처: review/20260314_140000_auto-review.md

### [Low] main 히스토리 문서 전용 커밋 squash
- **위치**: main 브랜치 git 히스토리
- **상태**: 미구현
- **배치**: Wave 배정 보류
- **설명**: docs 전용 커밋이 전체의 30~40% 차지. interactive rebase로 인접 문서 커밋 squash 정리 필요 — 출처: 코드 리뷰 피드백

### [Low] plans-progress 추적성 갭 2건
- **위치**: `docs/` plans, progress
- **상태**: 미구현
- **배치**: Wave 배정 보류
- **설명**: docs-consolidation, roadmap-redesign 계획서에 대응하는 progress 문서 없음 (초기 레거시) — 출처: review/20260314_140000_auto-review.md

---

## 단위 테스트

### [Medium] ConnectionHandler 단위 테스트 부재
- **위치**: `apex_core/` ConnectionHandler
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: 다수 리뷰에서 반복 지적, E2E 간접 커버 중. 상당한 작업량 — 출처: review/20260314_140000_auto-review.md, review/20260313_005117_auto-review.md (v0.5)

### [Medium] PgTransaction begun_ 경로 unit test 불가
- **위치**: `apex_shared/` PgTransaction
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: begin()이 async이므로 mock PgConnection 또는 integration test 필요 — 출처: review/20260314_090000_auto-review.md (v0.5)

### [Medium] Server 라이프사이클 에러 경로 테스트
- **위치**: `apex_core/` Server
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: 별도 태스크로 분리 — 출처: review/20260314_090000_auto-review.md (v0.5)

### [Medium] RedisMultiplexer 코루틴 명령 테스트
- **위치**: `apex_shared/` RedisMultiplexer
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: async 테스트 인프라 필요 — 출처: review/20260314_140000_auto-review.md (v0.5)

### [Medium] Session async_recv 테스트
- **위치**: `apex_core/` Session
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: 별도 태스크 — 출처: review/20260314_140000_auto-review.md (v0.5)

### [Medium] TC-1. Gateway/Auth/Chat 핵심 모듈 9개 테스트 부재
- **위치**: `apex_services/` (gateway, auth-svc, chat-svc)
- **상태**: 미구현
- **배치**: v0.5 이후 별도 테스트 인프라 구축
- **설명**: Mock 인프라(MockKafkaAdapter, MockRedisAdapter, MockPgAdapter, MockCoreEngine) 미구축이 근본 원인. 대상: message_router, response_dispatcher, gateway_pipeline, gateway_config_parser, broadcast_fanout, config_reloader, jwt_blacklist, auth_service, chat_service + rate_limit_facade, pubsub_listener — 출처: auto-review (reviewer-test-coverage)

---

## 서비스 체인

### [Medium] pgbouncer md5 auth
- **위치**: `apex_infra/` pgbouncer 설정
- **상태**: 미구현
- **배치**: v0.6 (운영 인프라)
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
