# v0.5 Wave 2 Auto-Review 백로그

> auto-review에서 에스컬레이션된 미해결 이슈. 다음 세션에서 사용자와 논의 후 대응.

## Critical (3건)

### C-1. PerIpRateLimiter TTL 콜백 미연결
- **위치**: `apex_shared/lib/rate_limit/src/per_ip_rate_limiter.cpp`
- **내용**: TimingWheel on_expire → remove_entry 연결 부재. 만료 엔트리 메모리 잔존.
- **원인**: core/shared 경계 설계 결정 필요 (TimingWheel은 core, PerIpRateLimiter는 shared)
- **권장 해결 시점**: v0.5 Wave 2 패치 (머지 후 즉시)
- **발견자**: reviewer-memory

### C-2. ResponseDispatcher::on_response() 데이터 레이스
- **위치**: `apex_services/gateway/src/response_dispatcher.cpp`
- **내용**: Kafka 스레드에서 per-core PendingRequestsMap 직접 접근. cross_core_post 위임 설계 필요.
- **추가 영향**: 보안+정합성 (다른 세션에 응답 전달 가능)
- **권장 해결 시점**: v0.5 Wave 2 패치 (머지 후 즉시)
- **발견자**: reviewer-concurrency + reviewer-cross-cutting

### C-3. RefreshTokenRequest(msg_id=10) 라우팅 불가
- **위치**: `apex_services/gateway/gateway.toml`
- **내용**: gateway.toml routes에 시스템 범위([0,999]) 누락. 시스템 메시지별 라우팅 분기 설계 필요.
- **권장 해결 시점**: v0.5 Wave 2 패치
- **발견자**: reviewer-cross-cutting

## Important (11건)

### I-1. ChannelSessionMap 무제한 메모리 증가
- **위치**: `apex_services/gateway/include/apex/gateway/channel_session_map.hpp`
- **내용**: per-session 구독 제한 정책 미설정. 비즈니스 결정 필요.
- **발견자**: reviewer-memory

### I-2. GatewayPipeline::set_rate_limiter() 동시성 보호 없음
- **위치**: `apex_services/gateway/include/apex/gateway/gateway_pipeline.hpp`
- **내용**: atomic 또는 strand 보장 필요. ConfigReloader 활성화 시 실제 레이스.
- **발견자**: reviewer-concurrency + reviewer-cross-cutting

### I-3. review 문서 2개 상세 내용 부재
- **위치**: `docs/apex_common/review/20260315_210204_v0.5-wave1-phase*.md`
- **내용**: 원본 데이터 없이 복원 불가.
- **발견자**: reviewer-docs-records

### I-4. FileWatcher 테스트 flaky 위험
- **위치**: `apex_services/gateway/tests/test_file_watcher.cpp`
- **내용**: sleep 기반, CI 환경 의존. clock injection 향후 개선.
- **발견자**: reviewer-test-quality

### I-5. PendingRequests 테스트 flaky 위험
- **위치**: `apex_services/gateway/tests/test_pending_requests.cpp`
- **내용**: sleep 기반, 시간 injection 미지원.
- **발견자**: reviewer-test-quality

### I-6. gateway.toml JWT secret 하드코딩
- **위치**: `apex_services/gateway/gateway.toml`
- **내용**: 환경변수 치환 메커니즘 필요.
- **발견자**: reviewer-security

### I-7. SQL 마이그레이션 기본 비밀번호
- **위치**: `apex_services/auth-svc/migrations/`
- **내용**: 인프라 시크릿 주입 전략 필요.
- **발견자**: reviewer-security

### I-8. session_store Redis 명령 인젝션
- **위치**: `apex_services/auth-svc/src/session_store.cpp`
- **내용**: RedisMultiplexer printf-style API 변경 필요.
- **발견자**: reviewer-security

### I-9. PubSub→클라이언트 프레임 포맷 불일치
- **위치**: `apex_services/gateway/src/broadcast_fanout.cpp`
- **내용**: build_pubsub_payload vs WireHeader 계약 위반. placeholder이지만 구현 시 수정 필요.
- **발견자**: reviewer-cross-cutting

### I-10. redis.ratelimit TOML 섹션 파싱 누락
- **위치**: `apex_services/gateway/src/gateway_config_parser.cpp`
- **내용**: GatewayConfig + parser 변경 필요.
- **발견자**: reviewer-cross-cutting

### I-11. RateLimitEndpointConfig → EndpointRateConfig 변환 경로 부재
- **위치**: `apex_services/gateway/src/config_reloader.cpp`
- **내용**: hot-reload 경로 단절.
- **발견자**: reviewer-cross-cutting

## Minor (1건)

### m-1. user_id 파라미터 미사용
- **위치**: `apex_services/gateway/src/message_router.cpp`
- **내용**: MessageRouter + MetadataPrefix에 user_id 전달 경로 없음. 설계-구현 갭.
- **발견자**: reviewer-cross-cutting

## 테스트 커버리지 (별도)

### TC-1. Gateway/Auth/Chat 핵심 모듈 9개 테스트 부재
- **내용**: Mock 인프라(MockKafkaAdapter, MockRedisAdapter, MockPgAdapter, MockCoreEngine) 미구축이 근본 원인.
- **대상**: message_router, response_dispatcher, gateway_pipeline, gateway_config_parser, broadcast_fanout, config_reloader, jwt_blacklist, auth_service, chat_service + rate_limit_facade, pubsub_listener
- **권장 해결 시점**: v0.5 Wave 2 이후 별도 테스트 인프라 구축 작업
- **발견자**: reviewer-test-coverage
