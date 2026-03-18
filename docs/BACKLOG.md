# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 48

---

## NOW

### #1. apex_core 프레임워크 내부 아키텍처 문서 + 구조도
- **등급**: CRITICAL
- **스코프**: core, docs
- **타입**: docs
- **설명**: 서비스 개발자가 이 문서만 보고 새 서비스를 프레임워크 위에 올릴 수 있도록 하는 통합 아키텍처 가이드. 현재는 Gateway/Auth/Chat 구현 코드를 직접 역추적해야 하며 반복 시행착오 발생. 포함 내용: Server/CoreEngine/Listener/ConnectionHandler 라이프사이클 + per-core 구조도, ServiceBase::bind_dispatcher → on_start 흐름 (PR #37에서 Phase 3.5 타이밍 수정됨), AdapterBase 초기화 순서, ResponseDispatcher 배선 (Gateway 전용), standalone CoreEngine 패턴. v0.6 서비스 온보딩의 선행 조건.

---

## IN VIEW

### #2. RedisMultiplexer cancel_all_pending UAF
- **등급**: CRITICAL
- **스코프**: shared
- **타입**: bug
- **설명**: `cancel_all_pending()`에서 timed_out 경로와 async_wait 경로 간 동기화 문제. resolver.cancel()이 completion handler를 post하여 코루틴을 resume시키는 시점에 PendingCommand가 다른 경로에서 destroy될 수 있어 UAF 발생 가능. `redis_multiplexer.cpp` 361-366 주석 참조.

### #3. Protocol concept Frame 내부 구조 미제약
- **등급**: CRITICAL
- **스코프**: core
- **타입**: design-debt
- **설명**: Protocol concept이 Frame의 내부 구조(header.msg_id, payload)를 명시적으로 요구하지 않음. ConnectionHandler가 `if constexpr requires`로 접근하므로, 다른 구조의 Frame을 가진 Protocol 구현 시 컴파일 오류 발생. accessor 메서드 요구 또는 Frame trait 도입 필요.

### #4. Assertion 크래시 시 __FUNCTION__ / __LINE__ 로깅
- **등급**: MAJOR
- **스코프**: core, infra
- **타입**: infra
- **설명**: assertion 실패 시 위치 정보 없이 서비스가 크래시됨. Windows SetUnhandledExceptionFilter + Linux SIGABRT/SIGILL 시그널 핸들러로 `__FUNCTION__`/`__LINE__` 로깅 필요. v0.6 디버깅 인프라.

### #5. gateway.toml 시크릿 운영 환경 관리
- **등급**: MAJOR
- **스코프**: infra, gateway
- **타입**: security
- **설명**: expand_env() 구현 완료, JWT RS256 전환으로 Gateway secret 불필요. 남은 과제: Redis 4인스턴스 비밀번호, PgBouncer 인증 정보의 프로덕션 시크릿 주입 전략 (Docker Secrets 또는 K8s ConfigMap).

### #6. SQL 마이그레이션 DB 역할 비밀번호 하드코딩
- **등급**: MAJOR
- **스코프**: infra, auth-svc
- **타입**: security
- **설명**: `apex_services/auth-svc/migrations/001_create_schema_and_role.sql`에 평문 비밀번호 `'auth_secret_change_me'` 하드코딩. WARNING 주석 있으나 환경 변수 치환 미구현. v0.6 시크릿 주입 전략으로 해결 필요.

### #7. Linux CI Sanitizer 파이프라인 확장
- **등급**: MAJOR
- **스코프**: ci
- **타입**: infra
- **설명**: CI에 linux-asan, linux-tsan job 존재 (CMakePresets). UBSAN은 ASAN에 통합됨. Valgrind memcheck 미추가. suppression 파일 운용 중 (`tsan_suppressions.txt`, `lsan_suppressions.txt`). Valgrind job 추가 검토.

### #8. Redis 4인스턴스 무인증 + PgBouncer 평문 비밀번호
- **등급**: MAJOR
- **스코프**: infra
- **타입**: security
- **설명**: 로컬 개발 환경에서 Redis 4인스턴스(auth/pubsub/ratelimit/chat) 무인증, PgBouncer md5 평문 인증. 프로덕션 v0.6 배포 전 Redis ACL + PgBouncer SCRAM-SHA-256 + 시크릿 주입 필수.

### #9. CI에서 Windows apex_shared 어댑터 빌드 미검증
- **등급**: MAJOR
- **스코프**: ci, shared
- **타입**: infra
- **설명**: CI Windows job이 apex_core만 빌드. apex_shared 어댑터(Kafka/Redis/PostgreSQL)는 Windows에서 미커버. 별도 job 추가 또는 루트 빌드 확장 필요.

### #10. CircuitBreaker HALF_OPEN 코루틴 인터리빙
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: should_allow()가 원자적이지 않아, HALF_OPEN 상태에서 복수 코루틴이 동시 진입 시 max_calls 초과 가능. 현재 어댑터 통합 미완으로 미트리거 상태. 통합 시점에 atomic CAS 또는 should_allow() 내 선증가 방식으로 수정 필요.

### #11. CircuitBreaker::call() Result<void> 타입 제한
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: CircuitBreaker template call()이 Result<void>만 지원하도록 concept 제약됨. 실제 어댑터 사용 시 Result<T> 제네릭 확장 필요. 어댑터 통합 전에 설계/구현.

### #12. Server 예외 경로 소멸 순서
- **등급**: MAJOR
- **스코프**: core
- **타입**: bug
- **설명**: Server::run()에서 listener->start() 실패 시 이미 생성된 listeners_/services가 정리되지 않은 채 ~Server() 호출되어 dangling pointer 위험. exception-safe RAII 패턴 필요.

### #13. Listener<P> 단위 테스트 부재
- **등급**: MAJOR
- **스코프**: core
- **타입**: test
- **설명**: Listener의 start/drain/stop, per-core handler 동기화, acceptor 관리에 대한 단위 테스트 없음. E2E 간접 커버만 존재. multi-protocol dispatcher sync 로직 검증 필요.

### #14. WebSocketProtocol 테스트 부재
- **등급**: MAJOR
- **스코프**: shared
- **타입**: test
- **설명**: WebSocketProtocol::try_decode(), consume_frame() 단위 테스트 미작성. 현재 길이-접두어 방식 MVP. Beast 통합 시 완전한 WebSocket 프레임 디코딩 테스트 필수.

### #15. Server 라이프사이클 에러 경로 테스트
- **등급**: MAJOR
- **스코프**: core
- **타입**: test
- **설명**: Server::run()의 listener start 실패, drain timeout, shutdown edge case 등 에러 경로 테스트 미구현. 71/71 유닛은 정상 경로만 커버.

### #16. PgTransaction begun_ 경로 unit test
- **등급**: MAJOR
- **스코프**: shared
- **타입**: test
- **설명**: PgTransaction 테스트가 integration 수준(실제 PgPool 필요). begin()이 async이므로 MockPgConnection 필요. v0.6 테스트 인프라 개선 시 구축.

### #17. Gateway/Auth/Chat 핵심 모듈 테스트 부재
- **등급**: MAJOR
- **스코프**: gateway, auth-svc, chat-svc
- **타입**: test
- **설명**: 서비스 모듈(websocket_protocol, rate_limit_facade, broadcast_fanout, jwt_blacklist, pubsub_listener 등) 단위 테스트 부재. Mock 인프라(MockKafkaAdapter/MockRedisAdapter/MockPgAdapter) 미구축이 근본 원인. E2E 11/11로 간접 커버 중.

### #18. Mock thread-safety 불일치
- **등급**: MAJOR
- **스코프**: shared
- **타입**: test
- **설명**: Mock 객체의 thread-safety가 실제 구현체와 불일치. E2E fixture는 완성 및 동작 중. TSAN suppression 파일 운용. v0.6 테스트 정리 단계에서 감사 필요.

### #19. Auth/Chat 비즈니스 로직 단위 테스트 0건
- **등급**: MAJOR
- **스코프**: auth-svc, chat-svc
- **타입**: test
- **설명**: ~1500줄 핸들러 코드(on_login, on_join_room 등)에 단위 테스트 없음. E2E로만 검증. Mock 인프라 구축 후 격리 테스트 필요.

### #20. BumpAllocator / ArenaAllocator 벤치마크
- **등급**: MAJOR
- **스코프**: core
- **타입**: perf
- **설명**: malloc vs BumpAllocator vs ArenaAllocator 할당 성능 비교 벤치마크 미구현. SlabAllocator 벤치마크(`bench_slab_allocator.cpp`)는 존재. 측정 항목: 단일 할당 지연(ns/op), 반복 할당+리셋 사이클, 다양한 크기/정렬 조합, ArenaAllocator 블록 체이닝 오버헤드. v0.6 성능 최적화 판단의 기준 데이터.

---

## DEFERRED

### #21. Server multi-listener dispatcher sync_all_handlers
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: PR #37에서 default handler sync를 Phase 3.5로 이동하여 TCP multi-listener dispatch 수정됨. 개별 msg_id 핸들러(`register_handler`)는 여전히 primary listener만 적용. WebSocket 정식 지원 또는 멀티 프로토콜 시 `sync_all_handlers` 확장 필요.

### #22. async_send_raw + write_pump 동시 write 위험
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: `session.hpp`에 `@warning Only one async_send operation may be active at once` 문서화됨. 현재 write_pump만 사용하여 미트리거. 향후 API 확장 시 동기화 필요. private 전환 또는 concurrent access assert 검토.

### #23. TOCTOU: join_room SCARD→SADD 경합
- **등급**: MAJOR
- **스코프**: chat-svc
- **타입**: bug
- **설명**: `chat_service.cpp`에서 SCARD(방 인원 확인) → SADD(멤버 추가) 사이 경합 가능. Redis Lua script로 원자적 처리 필요하나 어댑터 Lua 지원 미구현. 실제 발생 빈도 극히 낮음 (max_members 초과 시 SADD 후 재검증으로 완화).

### #24. 어댑터 상태 관리 불일치
- **등급**: MINOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: KafkaAdapter는 자체 AdapterState, 나머지는 AdapterBase::ready_ 사용. 어댑터 인터페이스 정규화 시 통일 검토.

### #25. GatewayEnvelope FBS msg_id uint16 불일치
- **등급**: MINOR
- **스코프**: gateway, shared
- **타입**: design-debt
- **설명**: FBS 스키마 `gateway_envelope.fbs`에서 msg_id가 uint16이지만 코드(`kafka_envelope.hpp`)에서 uint32_t 사용. 런타임 영향 없음 (수동 직렬화 사용). 레거시 FBS 파일 삭제 또는 타입 정정 검토.

### #26. ReplyTopicHeader::serialize() silent failure
- **등급**: MINOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: overflow 시 빈 vector 반환하여 정상 케이스와 구분 불가. 테스트(SerializeOverflowReturnsEmpty) 존재하나 API 비대칭 (parse→expected vs serialize→vector). `std::expected` 반환으로 통일 검토. Kafka 토픽 249자 제한으로 실제 위험 극히 낮음.

### #27. FrameError→ErrorCode 매핑 구분 불가
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 프로토콜 파싱 에러가 모두 ErrorCode::InvalidMessage로 매핑. BodyTooLarge, HeaderParseError 등 세분화 없음. 디버깅/모니터링 개선 시 에러 코드 확장 검토.

### #28. drain_timeout 만료 시 Server 멤버 소멸 순서
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: finalize_shutdown()이 listeners→core_engine 순서로 정리하여 현재 안전. 명시적 reset() 추가로 견고화 가능하나 낮은 우선순위.

### #29. drain()/stop() 동일 구현
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: Listener에서 drain()과 stop()이 동일 구현 (acceptors 순회 stop). 의미적으로 drain=soft close, stop=hard close로 분리 검토. graceful shutdown 개선 시.

### #30. session.cpp clang-tidy 워닝 잔여분
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: server.cpp(1건), arena_allocator.cpp(8건) 수정 완료. session.cpp 5건 중 2건만 수정됨 (implicit-bool-conversion, owning-memory, coroutine-ref-param, unused-return-value).

### #31. make_socket_pair 반환 순서 불일치
- **등급**: MINOR
- **스코프**: core
- **타입**: bug
- **설명**: 대부분 테스트에서 `auto [server, client]` 사용하나 벤치마크(`bench_session_throughput.cpp`)에서 `auto [client, server]` 역순 사용. 의미론과 변수명 통일 필요.

### #32. CI docs-only 커밋에도 전체 빌드 실행
- **등급**: MINOR
- **스코프**: ci
- **타입**: infra
- **설명**: GitHub Actions `paths-ignore`가 PR 전체 diff 기준으로 평가. docs-only 커밋에도 전체 빌드 ~11분 소모. workaround: `[skip ci]` 커밋 메시지.

### #33. vcpkg.json 의존성 정리 + 버전 불일치
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: vcpkg.json version 필드가 0.4.0으로 실제 v0.5.5.1과 불일치. apex_shared/vcpkg.json 독립 빌드 의존성 누락. 빌드 영향 큼, 별도 태스크.

### #34. test_session.cpp 매직 넘버 256 하드코딩
- **등급**: MINOR
- **스코프**: core
- **타입**: test
- **설명**: 테스트에서 max_queue_depth_=256 하드코딩. named constant로 정의하면 유지보수성 향상.

### #35. test_redis_reply.cpp 매직 넘버 0
- **등급**: MINOR
- **스코프**: shared
- **타입**: test
- **설명**: empty array 테스트에서 매직 넘버 정리 미완료. 코드 위생 개선.

### #36. Acceptor core 0 부하 불균형
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 단일 acceptor가 `engine_.io_context(0)`에 바인딩되어 core 0에 on_accept 콜백 집중. Linux reuseport로 완화 가능하나 Windows에서는 문제 남음. per-core acceptor 검토.

### #37. cross-thread acceptor close
- **등급**: MINOR
- **스코프**: core
- **타입**: bug
- **설명**: TcpAcceptor가 io_context(0)에서 생성되지만 Listener::drain/stop이 다른 스레드에서 호출 가능성. 현재 control_io_ 스레드 호출로 위험 낮으나 재검토 필요.

### #38. boost-beast 조기 추가
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: vcpkg.json에 boost-beast가 추가되었으나 WebSocketProtocol MVP가 미사용. Beast 통합(v0.6+) 전까지 미사용 의존성.

### #39. CMakeLists.txt 하드코딩 상대 경로
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: `apex_core/CMakeLists.txt`에 상대 경로 하드코딩. 빌드 이동성 개선을 위해 CMake 변수 활용 검토.

### #40. NUMA 바인딩 + Core Affinity
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 멀티 소켓 환경(NUMA)에서 원격 메모리 접근 지연 제거 + CPU 캐시 warm 유지를 위한 코어 바인딩. 싱글 소켓에서는 불필요. `numactl` 프로세스 레벨 바인딩으로 80% 커버 가능. v1.0+ 멀티 소켓 배포 시 재평가.

### #41. mmap 직접 사용 (malloc 대체)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: BumpAllocator/ArenaAllocator 초기 chunk를 mmap(Linux)/VirtualAlloc(Windows)로 교체하여 madvise/mprotect 제어권 확보. 현재 기동 1회 할당이므로 런타임 차이 0. v0.6 RSS 모니터링 도입 시 재평가.

### #42. Hugepage (대형 페이지)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 2MB 페이지로 TLB 엔트리 사용량 대폭 감소. 워크로드에 따라 5~30% 개선 가능. Linux THP 자동 적용 가능하나 명시적 판단은 부하 테스트에서 TLB miss 병목 확인 후.

### #43. L1 로컬 캐시
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: per-core 핫 데이터 캐싱. shared-nothing 설계로 캐시 미스 이미 최소화. 부하 테스트에서 병목 확인 시 도입. v1.0+ 이후.

### #44. 코루틴 프레임 풀 할당 (ADR-21)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: ADR-21 설계 완료. 현재 mimalloc/jemalloc HALO 최적화로 대응. 벤치마크에서 코루틴 프레임 힙 할당이 병목으로 확인될 경우 커스텀 promise_type 풀 오버로드 도입.

### #45. plans-progress 추적성 갭
- **등급**: MINOR
- **스코프**: docs
- **타입**: docs
- **설명**: docs-consolidation, roadmap-redesign 계획서에 대응 progress 부재 (초기 레거시). 최근 작업(Wave 1/2, E2E, service-layer)은 정상 추적. 전수 감사 시 해결.

### #46. auto-review 리뷰어 확장
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **설명**: reviewer-protocol(서비스 간 메시지 스키마), reviewer-adapter(Kafka/Redis/PG 패턴), reviewer-perf(핫패스/벤치마크 회귀) 3명 추가 예정. 현재 6명 리뷰어가 v0.5 범위에 적합.

### #47. README 빌드 안내 보강
- **등급**: MINOR
- **스코프**: docs
- **타입**: docs
- **설명**: README.md 빌드 안내 최소한. 상세 가이드는 `apex_core/CLAUDE.md`와 루트 `CLAUDE.md`에 존재. README 발견성(discoverability) 개선용.
