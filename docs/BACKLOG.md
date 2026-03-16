# BACKLOG

미해결 TODO/백로그 항목 집약. 새 작업 시작 전 반드시 확인.
완료된 항목은 즉시 삭제 (git에 이력 보존).

---

## Critical

### [Critical] C-1. JWT 알고리즘 불일치 (Auth RS256 vs Gateway HS256)
- **위치**: `apex_services/auth-svc/` (JWT 서명), `apex_services/gateway/` (JWT 검증)
- **상태**: 미구현
- **배치**: v0.5 패치 (보안 + cross-cutting)
- **설명**: Auth 서비스는 RS256으로 서명, Gateway는 HS256으로 검증 — 알고리즘 불일치로 토큰 검증 실패. JwtVerifier에 알고리즘 분기 + 공개키 파일 로딩 필요 — 출처: auto-review 에스컬레이션

### [Critical] C-2. Kafka response topic 미스라우팅
- **위치**: `apex_services/auth-svc/`, `apex_services/chat-svc/`, `apex_services/gateway/`
- **상태**: 설계 결정 필요
- **배치**: v0.5 패치 (cross-cutting)
- **설명**: Auth→`auth.responses`, Chat→`chat.responses`로 응답하나 Gateway는 `gateway.responses`만 consume. 전체 요청-응답 체인 단절. 설계 결정 필요: (A) 서비스가 `gateway.responses`로 통일 vs (B) Gateway가 다중 토픽 consume — 출처: auto-review 에스컬레이션

### [Critical] C-3. PerIpRateLimiter TTL 콜백 미연결
- **위치**: `apex_shared/lib/rate_limit/src/per_ip_rate_limiter.cpp`
- **상태**: 미구현
- **배치**: v0.5 Wave 2 패치 (머지 후 즉시)
- **설명**: TimingWheel on_expire → remove_entry 연결 부재. 만료 엔트리 메모리 잔존. core/shared 경계 설계 결정 필요 (TimingWheel은 core, PerIpRateLimiter는 shared) — 출처: auto-review (reviewer-memory)

### [Critical] C-4. ResponseDispatcher::on_response() 데이터 레이스
- **위치**: `apex_services/gateway/src/response_dispatcher.cpp`
- **상태**: 미구현
- **배치**: v0.5 Wave 2 패치 (머지 후 즉시)
- **설명**: Kafka 스레드에서 per-core PendingRequestsMap 직접 접근. cross_core_post 위임 설계 필요. 보안+정합성 영향 (다른 세션에 응답 전달 가능) — 출처: auto-review (reviewer-concurrency + reviewer-cross-cutting)

### [Critical] C-5. RefreshTokenRequest(msg_id=10) 라우팅 불가
- **위치**: `apex_services/gateway/gateway.toml`
- **상태**: 미구현
- **배치**: v0.5 Wave 2 패치
- **설명**: gateway.toml routes에 시스템 범위([0,999]) 누락. 시스템 메시지별 라우팅 분기 설계 필요 — 출처: auto-review (reviewer-cross-cutting)

---

## Important

### [Important] I-1. JWT claim 불일치 (sub/jti)
- **위치**: `apex_services/auth-svc/` (JWT 생성), `apex_services/gateway/` (JWT 파싱)
- **상태**: 미구현
- **배치**: v0.5 패치 (cross-cutting)
- **설명**: Auth는 sub=email, jti 미포함. Gateway는 sub→username으로 사용, jti 기반 블랙리스트. jti 미생성으로 로그아웃 토큰 무효화 불가 — 출처: auto-review 에스컬레이션

### [Important] I-2. ChannelSessionMap 무제한 메모리 증가
- **위치**: `apex_services/gateway/include/apex/gateway/channel_session_map.hpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: per-session 구독 제한 정책 미설정. 비즈니스 결정 필요 — 출처: auto-review (reviewer-memory)

### [Important] I-3. GatewayPipeline::set_rate_limiter() 동시성 보호 없음
- **위치**: `apex_services/gateway/include/apex/gateway/gateway_pipeline.hpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: atomic 또는 strand 보장 필요. ConfigReloader 활성화 시 실제 레이스 — 출처: auto-review (reviewer-concurrency + reviewer-cross-cutting)

### [Important] I-4. review 문서 2개 상세 내용 부재
- **위치**: `docs/apex_common/review/20260315_210204_v0.5-wave1-phase*.md`
- **상태**: 미구현
- **배치**: 별도 판단 (원본 데이터 없이 복원 불가)
- **설명**: 원본 데이터 없이 복원 불가 — 출처: auto-review (reviewer-docs-records)

### [Important] I-5. FileWatcher 테스트 flaky 위험
- **위치**: `apex_services/gateway/tests/test_file_watcher.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: sleep 기반, CI 환경 의존. clock injection 향후 개선 — 출처: auto-review (reviewer-test-quality)

### [Important] I-6. PendingRequests 테스트 flaky 위험
- **위치**: `apex_services/gateway/tests/test_pending_requests.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: sleep 기반, 시간 injection 미지원 — 출처: auto-review (reviewer-test-quality)

### [Important] I-7. gateway.toml JWT secret 하드코딩 + TOML 셸 환경변수 치환 미작동
- **위치**: `apex_services/gateway/gateway.toml`, TOML 파서 전반
- **상태**: 미구현
- **배치**: v0.6 (운영 인프라)
- **설명**: 환경변수 치환 메커니즘 필요. 추가 발견: `${JWT_SECRET:-...}` 구문을 TOML 파서가 처리하지 않아 셸 환경변수 치환이 실제로는 미작동 — 출처: auto-review (reviewer-security) + 에스컬레이션

### [Important] I-8. SQL 마이그레이션 DB 역할 비밀번호 하드코딩
- **위치**: `apex_services/auth-svc/migrations/`
- **상태**: 미구현
- **배치**: v0.6 (운영 인프라, 시크릿 매니저 도입 시 해결)
- **설명**: migration SQL에 평문 비밀번호. 인프라 시크릿 주입 전략 필요 — 출처: auto-review (reviewer-security) + 에스컬레이션

### [Important] I-9. session_store Redis 명령 인젝션
- **위치**: `apex_services/auth-svc/src/session_store.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: RedisMultiplexer printf-style API 변경 필요 — 출처: auto-review (reviewer-security)

### [Important] I-10. PubSub→클라이언트 프레임 포맷 불일치
- **위치**: `apex_services/gateway/src/broadcast_fanout.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: build_pubsub_payload vs WireHeader 계약 위반. placeholder이지만 구현 시 수정 필요 — 출처: auto-review (reviewer-cross-cutting)

### [Important] I-11. redis.ratelimit TOML 섹션 파싱 누락
- **위치**: `apex_services/gateway/src/gateway_config_parser.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: GatewayConfig + parser 변경 필요 — 출처: auto-review (reviewer-cross-cutting)

### [Important] I-12. RateLimitEndpointConfig → EndpointRateConfig 변환 경로 부재
- **위치**: `apex_services/gateway/src/config_reloader.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: hot-reload 경로 단절 — 출처: auto-review (reviewer-cross-cutting)

### [Important] I-13. async_send_raw + write_pump 동시 write 위험
- **위치**: `apex_core/` Session (async_send_raw, write_pump)
- **상태**: 미구현 (현재 코드 경로에서 미트리거)
- **배치**: v0.5 패치
- **설명**: async_send_raw와 write_pump가 동시에 소켓 write를 시도할 수 있는 구조. 현재 코드 경로에서는 트리거되지 않지만, 향후 확장 시 위험 — 출처: auto-review (reviewer-concurrency)

### [Important] I-14. GatewayEnvelope FBS msg_id uint16 불일치 (코드 uint32)
- **위치**: `apex_services/gateway/` FlatBuffers 스키마 + 코드
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: GatewayEnvelope FBS에서 msg_id가 uint16으로 정의되어 있으나 코드에서 uint32로 사용. 타입 불일치 — 출처: auto-review (reviewer-architecture)

### [Important] I-15. docs/Apex_Pipeline.md WireHeader v1(10B) vs 코드 v2(12B) 설계 문서 미갱신
- **위치**: `docs/Apex_Pipeline.md`, `apex_core/` WireHeader
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: 설계 문서에 WireHeader v1(10B)로 기재, 코드는 v2(12B)로 진화. 문서 갱신 필요 — 출처: auto-review (reviewer-architecture)

---

## Minor

### [Minor] m-1. user_id 파라미터 미사용
- **위치**: `apex_services/gateway/src/message_router.cpp`
- **상태**: 미구현
- **배치**: v0.5 패치
- **설명**: MessageRouter + MetadataPrefix에 user_id 전달 경로 없음. 설계-구현 갭 — 출처: auto-review (reviewer-cross-cutting)

### [Minor] m-2. 별도 백로그 파일 2건 미이전
- **위치**: `docs/apex_core/backlog_memory_os_level.md`, `docs/` 내 `20260315_094300_backlog.md`
- **상태**: 미구현
- **배치**: 즉시 (문서 정리)
- **설명**: `backlog_memory_os_level.md` → BACKLOG.md 리네이밍/통합 필요, `20260315_094300_backlog.md` → BACKLOG.md로 이전. 규칙: 별도 백로그 파일 생성 금지 — 출처: auto-review (reviewer-docs-records)

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

### [Medium] TC-1. Gateway/Auth/Chat 핵심 모듈 9건+ 테스트 부재
- **위치**: `apex_services/` (gateway, auth-svc, chat-svc)
- **상태**: 미구현
- **배치**: v0.5 이후 별도 테스트 인프라 구축
- **설명**: Mock 인프라(MockKafkaAdapter, MockRedisAdapter, MockPgAdapter, MockCoreEngine) 미구축이 근본 원인. 대상: websocket_protocol, rate_limit_facade, redis_connection, broadcast_fanout, jwt_blacklist, pubsub_listener, auth_service, chat_db_consumer, gateway_pipeline 코루틴 경로 + message_router, response_dispatcher, gateway_config_parser, config_reloader — 출처: auto-review (reviewer-test-coverage) + 에스컬레이션

### [Medium] TC-2. Mock thread-safety 불일치 + E2E fixture 미구현 + suppression 파일 중복
- **위치**: `apex_services/` 테스트 인프라
- **상태**: 미구현
- **배치**: v0.5 이후
- **설명**: Mock 객체의 thread-safety가 실제 구현체와 불일치, E2E fixture 미구현, 테스트 suppression 파일 중복 — 출처: auto-review (reviewer-test-quality)

---

## 서비스 체인

### [Medium] Redis 4인스턴스 무인증 + PgBouncer 평문 비밀번호
- **위치**: `apex_infra/` Redis 설정, pgbouncer 설정
- **상태**: 미구현
- **배치**: v0.6 (운영 인프라, 프로덕션 배포 시 설정 변경 필요)
- **설명**: 로컬 개발 환경에서 Redis 4인스턴스 무인증, PgBouncer md5 평문 비밀번호. 프로덕션 배포 전 인증 설정 필수 — 출처: review/20260314_140000_auto-review.md + 에스컬레이션

---

## 운영 인프라

### [Medium] CI에서 Windows apex_shared 어댑터 빌드 미검증
- **위치**: `.github/` CI 워크플로우
- **상태**: 미구현
- **배치**: Wave 3 — F. 운영 인프라
- **설명**: CI가 apex_core만 빌드, apex_shared 어댑터는 Windows에서 미커버 — 출처: review/20260314_140000_auto-review.md (v0.5)

---

## 빌드 / 의존성

### [Low] vcpkg.json 의존성 정리 + 버전 불일치
- **위치**: `vcpkg.json`, `apex_shared/vcpkg.json`
- **상태**: 미구현
- **배치**: Wave 배정 보류 (빌드 영향 큼, 별도 판단)
- **설명**: 빌드 영향 큼, 별도 태스크. 추가: vcpkg.json 버전이 0.4.0으로 실제 v0.5.4.1과 불일치, apex_shared/vcpkg.json에 독립 빌드 의존성 누락 — 출처: review/20260314_140000_auto-review.md + auto-review (reviewer-infra)

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
