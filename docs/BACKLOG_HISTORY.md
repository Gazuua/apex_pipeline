# BACKLOG HISTORY

완료된 백로그 항목 아카이브. 최신 항목이 파일 상단.
모든 이슈는 BACKLOG.md 경유 필수 — 히스토리 직접 생성 금지.

<!-- NEW_ENTRY_BELOW -->

### #67. server.global<T>() / ConsumerPayloadPool / wire_services() 단위 테스트
- **등급**: MAJOR | **스코프**: core, shared | **타입**: test
- **해결**: 2026-03-21 18:15:55 | **방식**: FIXED | **커밋**: 2988f93
- **비고**: server.global<T>() 4 TC, ConsumerPayloadPool 7 TC, wire_services() 2 TC. Batch B 코어/공유 테스트 소탕 일괄 작업.

### #101. ErrorSender::build_error_frame service_error_code 라운드트립 테스트
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-21 18:15:55 | **방식**: FIXED | **커밋**: 2988f93
- **비고**: service_error_code 0 및 비0 값 FlatBuffers 직렬화/역직렬화 라운드트립 4 TC.

### #13. Listener<P> 단위 테스트 부재
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-21 18:15:55 | **방식**: FIXED | **커밋**: 2988f93
- **비고**: start/drain/stop 라이프사이클, per-core handler 동기화, acceptor 관리 7 TC.

### #14. WebSocketProtocol 테스트 부재
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-21 18:15:55 | **방식**: FIXED | **커밋**: 2988f93
- **비고**: try_decode/consume_frame 7 TC. 정상/불완전/멀티프레임 시나리오.

### #16. PgTransaction begun_ 경로 unit test
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-21 18:15:55 | **방식**: FIXED | **커밋**: 2988f93
- **비고**: MockPgConn 기반 13 TC. pool_ 미사용 멤버 제거 + finished_ 가드 추가 (auto-review 발견).

### #103. KafkaMessageMeta.session_id SessionId 강타입화
- **등급**: MINOR | **스코프**: core, shared | **타입**: design-debt
- **해결**: 2026-03-21 17:39:51 | **방식**: FIXED
- **비고**: SessionId를 session_id.hpp로 분리. KafkaMessageMeta.session_id를 SessionId 강타입으로 변경. KafkaDispatchBridge에서 make_session_id() 변환. 서비스 send_response 시그니처 SessionId 전파.

### #97. 서비스 레이어 코드 위생 일괄 정리 (잔여 2건)
- **등급**: MINOR | **스코프**: gateway, auth-svc, chat-svc | **타입**: design-debt
- **해결**: 2026-03-21 17:39:51 | **방식**: FIXED
- **비고**: ⑦ Session::max_queue_depth_ 256 하드코딩 → ServerConfig.session_max_queue_depth 설정화 (ServerConfig→SessionManager→Session 전파). ① ParsedConfig 중복은 WONTFIX (Auth/Chat 구조 상이 — 공통화 시 오히려 복잡도 증가).

### #105. Chat join_room SCARD/SADD TOCTOU 레이스
- **등급**: MAJOR | **스코프**: chat-svc | **타입**: bug
- **해결**: 2026-03-21 16:54:51 | **방식**: FIXED | **커밋**: 3b7f2ef
- **비고**: Redis Lua 스크립트로 SISMEMBER+SCARD+조건SADD 원자적 처리. 기존 5단계 비원자적 호출을 단일 EVAL로 통합.

### #4. Assertion 크래시 시 __FUNCTION__ / __LINE__ 로깅
- **등급**: MAJOR | **스코프**: core, infra | **타입**: infra
- **해결**: 2026-03-21 16:54:51 | **방식**: FIXED | **커밋**: aedeb8e
- **비고**: APEX_ASSERT 매크로(source_location 기반) + 크래시 시그널 핸들러(SIGABRT/SIGSEGV/SIGFPE/SIGBUS) 도입. Windows SEH 지원. sanitizer 빌드 자동 감지하여 no-op.

### #109. 벤치마크 빌드 큐잉 시스템 강제
- **등급**: MAJOR | **스코프**: tools | **타입**: infra
- **해결**: 2026-03-21 15:43:39 | **방식**: FIXED
- **비고**: queue-lock.sh에 benchmark 서브커맨드 추가 (build 채널 공유, per-execution lock). validate-build.sh hook에 bench_* 직접 실행 차단. 벤치마크 README 워크플로우 queue-lock 경유로 갱신.

### #107. v0.5.10.0 벤치마크 실행 및 보고서 작성
- **등급**: MAJOR | **스코프**: core, tools | **타입**: perf
- **해결**: 2026-03-21 14:37:49 | **방식**: FIXED | **커밋**: 4c8e5ee
- **비고**: 벤치마크 보고서 v2 전면 재설계 — Release vs Debug 비교 → 버전 간 비교 + 7개 방법론 비교 체계. PDF→HTML(Plotly 인터랙티브, 다크 대시보드) 전환. 신규 벤치마크 6종. Per-core vs Shared 아키텍처 비교에서 4워커 2.1x 우위 확인. GitHub Pages 배포 + README 대문 차트.

### #111. 주요 지침 문서 정리
- **등급**: MAJOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-21 12:30:10 | **방식**: FIXED
- **비고**: CLAUDE.md 6개 파일 컴팩션 502→393줄 (21.7% 감소). 원본 참조 전환, 중복 제거, 간결화

### #110. 주요 작업 종류·흐름 정의 및 강제
- **등급**: MAJOR | **스코프**: tools, docs | **타입**: infra
- **해결**: 2026-03-21 12:30:10 | **방식**: FIXED
- **비고**: 7단계 워크플로우 (착수→설계→구현→검증→리뷰→문서갱신→머지) + 스킵 조건 정의. 루트 CLAUDE.md에 추가

### #108. 브랜치 핸드오프 시스템 테스트
- **등급**: MAJOR | **스코프**: tools | **타입**: infra
- **해결**: 2026-03-21 00:35:49 | **방식**: FIXED | **커밋**: 7ca7d96
- **비고**: 버그 4건 수정 + hook 기반 강제화 3종 구현 + CLAUDE.md 지침 정리

### #106. CrossCoreDispatcher MPSC→SPSC all-to-all mesh 리팩토링
- **등급**: MAJOR | **스코프**: core | **타입**: perf
- **해결**: 2026-03-20 21:30:00 | **방식**: FIXED | **커밋**: 134ce1e
- **비고**: —

### #23. TOCTOU: join_room SCARD→SADD 경합
- **등급**: MAJOR | **스코프**: chat-svc | **타입**: bug
- **해결**: 2026-03-20 18:13:49 | **방식**: DUPLICATE
- **비고**: #105와 중복. #105(IN VIEW)에 통합.

### #99. Nightly Valgrind 첫 실행 결과 확인
- **등급**: MAJOR | **스코프**: ci | **타입**: infra
- **해결**: 2026-03-20 10:26:12 | **방식**: FIXED
- **비고**: valgrind-unit: `include(CTest)` 추가로 DartConfiguration.tcl 생성 + 자체 빌드(아티팩트 의존 제거). valgrind-e2e: `gateway_e2e_valgrind.toml`(request_timeout 30s) + E2E 타임아웃 확대 + Kafka rebalance 30s 대기 + 스트레스 연결/메시지 축소. 3-job 병렬 구조(valgrind-unit, build, valgrind-e2e).

### #98. CI E2E 타이밍 민감 테스트 안정화
- **등급**: MAJOR | **스코프**: ci, e2e | **타입**: test
- **해결**: 2026-03-20 10:26:12 | **방식**: FIXED
- **비고**: `access_token_ttl_sec` 30→10초, RefreshTokenRenewal sleep 31→11초, TcpClient recv 기본 타임아웃 10→30초. ci.yml E2E에서 `--gtest_filter` 제거하여 11개 전체 테스트 실행.

### #91. SessionId 강타입 부재 (uint64_t typedef)
- **등급**: MAJOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-20 13:13:31 | **방식**: FIXED | **커밋**: ff22aa0
- **비고**: `using SessionId = uint64_t` → `enum class SessionId : uint64_t {}` 강타입화 + hash + fmt::formatter 구현

### #89. core → shared 역방향 의존 해소 (forwarding header + kafka_envelope)
- **등급**: CRITICAL | **스코프**: core, shared | **타입**: design-debt
- **해결**: 2026-03-20 13:13:31 | **방식**: FIXED | **커밋**: ea83a9b
- **비고**: forwarding header 제거 + FrameType concept 도입으로 core→shared 역방향 의존 해소

### #3. Protocol concept Frame 내부 구조 미제약
- **등급**: CRITICAL | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-20 13:13:31 | **방식**: FIXED | **커밋**: ea83a9b
- **비고**: FrameType concept 도입으로 Frame 내부 구조(msg_id, payload 등) 명시적 제약 추가

### #66. wire_services() co_spawn(detached) → spawn() tracked API 전환
- **등급**: MAJOR | **스코프**: shared, core | **타입**: design-debt
- **해결**: 2026-03-20 13:13:31 | **방식**: FIXED | **커밋**: 0344eda
- **비고**: CoreEngine spawn_tracked API 도입, co_spawn(detached) 우회 제거

### #56. 서비스 레이어 가드레일 — 코어 인터페이스 캡슐화 + 원칙 위반 방지
- **등급**: MAJOR | **스코프**: core, shared | **타입**: design-debt
- **해결**: 2026-03-20 13:13:31 | **방식**: FIXED | **커밋**: 0344eda
- **비고**: ServiceBase io_context 캡슐화로 서비스 레이어에서 직접 접근 차단

### #90. ErrorCode에 Gateway 전용 에러 코드 혼입
- **등급**: MAJOR | **스코프**: core, gateway | **타입**: design-debt
- **해결**: 2026-03-20 13:13:31 | **방식**: FIXED | **커밋**: 0ad23d8
- **비고**: Gateway 전용 에러 코드를 서비스별 자체 enum으로 분리, core ErrorCode에서 제거

### #2. RedisMultiplexer cancel_all_pending UAF
- **등급**: CRITICAL | **스코프**: shared | **타입**: bug
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: `cancelled` 플래그 + `static_on_reply()` early-return 가드 추가로 timed_out 경로-async_wait 경로 간 UAF 방지

### #10. CircuitBreaker HALF_OPEN 코루틴 인터리빙
- **등급**: MAJOR | **스코프**: shared | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: OPEN→HALF_OPEN 전환 시 `half_open_calls_ = 1` 초기화 + check-then-increment 패턴으로 코루틴 인터리빙 방어

### #11. CircuitBreaker::call() Result<void> 타입 제한
- **등급**: MAJOR | **스코프**: shared | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: `CircuitCallable` concept 도입 + `call()` 반환 타입 `std::invoke_result_t<F>` 제네릭화

### #68. GatewayService set_default_handler() 캡슐화 우회 해소
- **등급**: MAJOR | **스코프**: gateway | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: 83줄 인라인 람다 → `set_default_handler(&GatewayService::on_default_message)` + 멤버 함수 3개 추출 (`handle_authenticate_session`, `handle_subscribe_channel`, `handle_unsubscribe_channel`)

### #69. 프레임워크 가이드 Shutdown 시퀀스 갱신
- **등급**: MAJOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-20 10:30:35 | **방식**: DOCUMENTED
- **비고**: §3 Shutdown 시퀀스를 `finalize_shutdown()` 실제 구현 기준 7단계로 갱신 (Listener stop, Scheduler stop, Adapter drain/close 분리 반영)

### #70. 프레임워크 가이드 코드 예시 ServiceRegistry API 수정
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-20 10:30:35 | **방식**: DOCUMENTED
- **비고**: §4.1 `ctx.local_registry.get<T>()` → `ctx.local_registry.find<T>()` 변경 + API 주석 추가

### #71. PubSubListener mutex 예외 가이드 명시
- **등급**: MINOR | **스코프**: docs, gateway | **타입**: docs
- **해결**: 2026-03-20 10:30:35 | **방식**: DOCUMENTED
- **비고**: §8 #2 금지 원칙에 hiredis dedicated-thread 패턴 예외 조항 추가

### #72. safe_parse_u64 Result<uint64_t> 반환 개선
- **등급**: MINOR | **스코프**: chat-svc | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: `safe_parse_u64` 반환 `Result<uint64_t>`로 변경, 호출부 10곳 에러 핸들링 패턴(early return/continue) 적용

### #92. WebSocket msg_id 바이트오더 미지정
- **등급**: MAJOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: `connection_handler.hpp` WebSocket msg_id 추출에 `ntohl()` 적용, TCP WireHeader와 big-endian 통일

### #93. config.hpp → server.hpp 순환 include 비용
- **등급**: MAJOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: `ServerConfig`를 `server_config.hpp`로 분리, `config.hpp`와 `server.hpp` 모두 경량 헤더 참조로 전환

### #94. outstanding_coros_ fetch_add 메모리 오더 비대칭
- **등급**: MAJOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: FIXED
- **비고**: `fetch_add(1, relaxed)` → `fetch_add(1, acq_rel)` 변경, `fetch_sub(1, release)` + `load(acquire)`와 대칭

### #95. apex_core/README.md 전면 갱신 (WireHeader 크기, 의존성, 프로토콜 위치)
- **등급**: MAJOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-20 10:30:35 | **방식**: DOCUMENTED
- **비고**: WireHeader 10→12바이트 v2, ProtocolBase → Protocol concept, TcpBinaryProtocol apex_shared 위치 반영, 의존성 목록 현행화

### #96. post_init_callback 문서 vs 코드 괴리
- **등급**: MAJOR | **스코프**: core, docs | **타입**: design-debt
- **해결**: 2026-03-20 10:30:35 | **방식**: DOCUMENTED
- **비고**: Apex_Pipeline.md v0.5.6.0 "post_init_callback 완전 제거" → "서비스 사용 제거 (프레임워크 API 유지)" 정정. 실제 3개 서비스 main.cpp은 이미 사용 해제 상태

### #7. Linux CI 파이프라인 확장 (E2E + UBSAN + Valgrind)
- **등급**: MAJOR | **스코프**: ci, infra | **타입**: infra
- **해결**: 2026-03-19 22:46:48 | **방식**: FIXED
- **비고**: UBSAN preset 추가, 서비스 Docker 컨테이너화, E2E CI 통합, Nightly Valgrind (unit+E2E+스트레스 12개), workflow_dispatch 수동 트리거

### #9. CI에서 Windows apex_shared 어댑터 빌드 미검증
- **등급**: MAJOR | **스코프**: ci, shared | **타입**: infra
- **해결**: 2026-03-19 22:46:48 | **방식**: FIXED
- **비고**: build matrix 전체를 루트 레벨 빌드로 전환하여 해결. build-root 중복 job 제거

### #73. Docker 로컬 리눅스 빌드 스크립트 + 빌드 락 연동
- **등급**: MAJOR | **스코프**: tools, infra | **타입**: infra
- **해결**: 2026-03-19 21:42:41 | **방식**: SUPERSEDED
- **비고**: 멀티스테이지 Docker 이미지로 빌드를 말아넣는 방향으로 결정되어 로컬 Docker 빌드 스크립트 불필요. Docker Desktop WSL2의 OOM/좀비 컨테이너 문제(3회 재현)도 회피.

### #58. 코딩 컨벤션 확립 + .clang-format 도입 + 전체 일괄 포맷팅
- **등급**: MAJOR | **스코프**: core, shared, gateway, auth-svc, chat-svc, ci | **타입**: infra
- **해결**: 2026-03-19 20:40:08 | **방식**: FIXED | **커밋**: d9edce2
- **비고**: Allman brace + 120자 + 4칸 인덴트 `.clang-format` 설정. 전체 274 소스 파일 일괄 포맷팅. `.git-blame-ignore-revs` 등록. CI `format-check` job 추가 (`clang-format --dry-run --Werror`). clang-format 버전 21.1.8 고정.

### #54. 빌드/정적분석 경고 전수 소탕 + 경고 레벨 확립
- **등급**: MAJOR | **스코프**: core, shared, gateway, auth-svc, chat-svc, ci | **타입**: infra
- **해결**: 2026-03-19 20:40:08 | **방식**: FIXED | **커밋**: d9edce2
- **비고**: `cmake/ApexWarnings.cmake` — `apex_set_warnings()` 함수 정의 + 전 타겟 적용. MSVC `/W4 /WX`, GCC `-Wall -Wextra -Wpedantic -Werror`. designated initializer `{}` → 명시적 기본값, unused parameter, redundant-move, missing-field-initializers 전수 수정. TSAN atomic_thread_fence 억제.

### #62. FileWatcher::DetectsChange 간헐 실패 (Windows 타임스탬프 해상도)
- **등급**: MINOR | **스코프**: gateway | **타입**: bug
- **해결**: 2026-03-19 20:40:08 | **방식**: FIXED | **커밋**: 6ed6b67
- **비고**: 초기 파일 `last_write_time`을 2초 과거로 고정하여 NTFS 타임스탬프 캐싱 우회. sleep 없는 결정적 테스트 유지.

### #48. Post-E2E 코드 리뷰 (10개 관점)
- **등급**: CRITICAL | **스코프**: core, gateway, auth-svc, chat-svc, shared | **타입**: design-debt
- **해결**: 2026-03-19 14:58:14 | **방식**: FIXED
- **비고**: 4그룹 병렬 리뷰(Core-API/Architecture/Safety/Sweep) → 46건 발견. Phase 2: 즉시 수정 25건. Phase A: 코어 인프라 확장 D3(server.global&lt;T&gt;) + D2(wire_services 자동 배선) + D7(spawn tracked API). Phase B: GatewayGlobals 소유권 이관, post_init_callback 완전 제거, ChannelSessionMap per-core(shared_mutex 제거), send_error 헬퍼 11개 추출. Phase C: ConsumerPayloadPool. Auto-review 5명 → CRITICAL 4 + MAJOR 5 추가 수정(보안 취약점 1건 포함). 총 ~45파일 변경, 71/71 테스트 통과. 잔여 이슈 #66~#72로 분리 등록.

### #1. apex_core 프레임워크 가이드 (API 가이드 + 내부 아키텍처)
- **등급**: CRITICAL | **스코프**: core, docs | **타입**: docs
- **해결**: 2026-03-19 12:47:03 | **방식**: DOCUMENTED
- **비고**: 단일 파일 2레이어 구조 (`docs/apex_core/apex_core_guide.md`). 레이어 1(§1-§9): 서비스 스켈레톤, ServerConfig, 라이프사이클 Phase 1-3.5, 핸들러 4종, 어댑터, 메모리, 유틸리티, 금지사항 7종 BAD/GOOD, 빌드 CMake 템플릿. 레이어 2(§10): 컴포넌트 배치도, 요청/Kafka 흐름, ADR 포인터 10개. 부록(§11): 실전 패턴 4종. 아키텍처 결정 D1-D7 (#48 핸드오프 기반) 포함 — 코드 구현은 #48 담당.

### #60. 로그 디렉토리 구조 확립 + 경로 중앙화 + 파일명 표준화
- **등급**: MAJOR | **스코프**: core, gateway, auth-svc, chat-svc, infra | **타입**: infra
- **해결**: 2026-03-19 10:56:29 | **방식**: FIXED
- **비고**: async logger + daily_file_format_sink + exact_level_sink 조합. 서비스별/레벨별/날짜별 로그 분리, 프로젝트 루트 자동 탐지, service_name 검증, E2E 로그 경로 통합. 71/71 유닛 통과.

### #55. 로컬 빌드 큐잉 + 머지 직렬화 시스템 (Windows)
- **등급**: MAJOR | **스코프**: tools, infra | **타입**: infra
- **해결**: 2026-03-19 00:49:44 | **방식**: FIXED
- **비고**: `queue-lock.sh` 통합 스크립트 (FIFO 큐 + mkdir atomic lock + PID/timestamp stale 감지). PreToolUse hook으로 빌드/머지 우회 원천 차단. 테스트 26/26 + 빌드 71/71 통과.

### #15. Server 라이프사이클 에러 경로 테스트
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 22:21:52 | **방식**: FIXED
- **비고**: test_server_error_paths.cpp에 5개 TC 구현 완료 (포트 점유, 이중 run, shutdown, 재진입, listener 없는 run).

### #12. Server 예외 경로 소멸 순서
- **등급**: MAJOR | **스코프**: core | **타입**: bug
- **해결**: 2026-03-18 22:21:52 | **방식**: WONTFIX
- **비고**: Listener::start() 로컬 벡터 → move-assign 패턴으로 RAII 이미 적용. 중간 상태 불가능.

### #17. Gateway/Auth/Chat 핵심 모듈 테스트 부재
- **등급**: MAJOR | **스코프**: gateway, auth-svc, chat-svc | **타입**: test
- **해결**: 2026-03-18 22:21:52 | **방식**: FIXED
- **비고**: MockKafkaAdapter/MockPgPool 구축. test_message_router, test_auth_handlers, test_chat_handlers 구현 완료.

### #18. Mock thread-safety 불일치
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 22:21:52 | **방식**: FIXED
- **비고**: MockKafkaAdapter, MockPgPool 모두 std::mutex + std::lock_guard 적용 확인.

### #37. cross-thread acceptor close
- **등급**: MINOR | **스코프**: core | **타입**: bug
- **해결**: 2026-03-18 22:14:17 | **방식**: WONTFIX
- **비고**: Boost.Asio acceptor.close()는 스레드 안전. atomic running_ 플래그로 이중 호출 방지. 실제 버그 아님.

### #28. drain_timeout 만료 시 Server 멤버 소멸 순서
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 22:14:17 | **방식**: WONTFIX
- **비고**: C++ 멤버 선언 역순 소멸(RAII)으로 안전. shutdown_timer_ unique_ptr 자동 정리. 추가 조치 불필요.

### #35. test_redis_reply.cpp 매직 넘버 0
- **등급**: MINOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: nullptr reply type=0에 의도 설명 주석 추가.

### #34. test_session.cpp 매직 넘버 256 하드코딩
- **등급**: MINOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: kDefaultMaxQueueDepth named constant 도입.

### #33. vcpkg.json 의존성 정리 + 버전 불일치
- **등급**: MINOR | **스코프**: infra | **타입**: infra
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: version-semver 0.4.0 → 0.5.5 갱신. boost-beast는 v0.6 계획상 유지(#38).

### #31. make_socket_pair 반환 순서 불일치
- **등급**: MINOR | **스코프**: core | **타입**: bug
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: bench_helpers.hpp 반환 순서를 {server, client}로 통일. 호출부(bench_session_throughput.cpp) 동기 수정.

### #25. GatewayEnvelope FBS msg_id uint16 불일치
- **등급**: MINOR | **스코프**: gateway, shared | **타입**: design-debt
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: gateway_envelope.fbs msg_id를 uint16 → uint32로 수정. C++ 코드와 일치.

### #45. plans-progress 추적성 갭
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 21:48:57 | **방식**: SUPERSEDED
- **비고**: #59(문서 자동화)에 흡수. 레거시 소급 보정은 ROI 없음, 향후 pre-commit hook으로 강제 예정.

### #46. auto-review 리뷰어 확장
- **등급**: MINOR | **스코프**: tools | **타입**: infra
- **해결**: 2026-03-18 21:44:31 | **방식**: SUPERSEDED
- **비고**: 5→6→12→7명으로 진화 후 plugin 체제 최종 안착. 현재 7명 리뷰어가 v0.6까지 충분.

### #74. 전체 문서에서 특정 표현 완전 제거
- **등급**: MAJOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 21:10:25 | **방식**: FIXED
- **비고**: 대상 16건 전수 치환 완료. 7개 파일 수정. grep 제로 확인 완료.

### #75. 백로그 연관 링킹 필드 + 섹션 내 우선순위 규칙 + NOW 기준 확장
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 21:10:25 | **방식**: FIXED
- **비고**: docs/CLAUDE.md § 백로그 운영에 3건 반영 완료. 기존 연관 필드 양방향 검증 완료.

### #76. CI docs-only 커밋 빌드 스킵
- **등급**: MAJOR | **스코프**: ci | **타입**: infra
- **해결**: 2026-03-18 21:10:25 | **방식**: FIXED
- **비고**: dorny/paths-filter@v3 이미 구현됨 (.github/workflows/ci.yml). source 필터로 docs-only PR 자동 스킵.

### #77. session.cpp clang-tidy 워닝 잔여분
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 19:57:43 | **방식**: SUPERSEDED
- **비고**: #54 (빌드/정적분석 경고 전수 소탕)에 흡수.

### #78. main 히스토리 문서 전용 커밋 squash
- **등급**: MINOR | **스코프**: docs | **타입**: infra
- **해결**: 2026-03-18 12:53:47 | **방식**: SUPERSEDED
- **비고**: --squash merge 워크플로우가 이미 PR 단위로 처리. interactive rebase on main은 안전 규칙 위반.

### #79. 테스트 이름 오타 MoveConstruction 2건
- **등급**: MINOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: MoveConstruction은 정상 영어 (Move + Construction). 오타 아님.

### #80. Compaction / LSA (Log-Structured Allocator)
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: 현 아키텍처(bump+slab+arena)에서 외부 단편화 거의 없어 구조적 불필요. GB급 인메모리 캐시 도입 시만 재평가.

### #81. new_refresh_token E2E 테스트 미검증
- **등급**: MAJOR | **스코프**: auth-svc | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED | **커밋**: 98eca92
- **비고**: e2e_auth_test.cpp에서 token refresh flow + new_refresh_token 필드 검증 완료. 11/11 E2E 통과.

### #82. Session async_recv 테스트
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_session.cpp에 10+ 시나리오 (정상 read, frame buffering, EOF, 에러). 71/71 유닛 통과.

### #83. RedisMultiplexer 코루틴 명령 테스트
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_redis_adapter.cpp에서 async 명령 실행 + 에러 처리 검증 완료.

### #84. ConnectionHandler 단위 테스트 부재
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_connection_handler.cpp 추가. 9+ 테스트 (accept, read loop, dispatch, session lifecycle, multi-listener). 71/71 통과.

### #85. review 문서 2개 상세 내용 부재
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: v0.5 Wave 1 review 원본 데이터 없이 복원 불가. 초기 레거시.

### #86. E2E 테스트 실행 가이드 문서
- **등급**: MAJOR | **스코프**: docs, infra | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: apex_services/tests/e2e/CLAUDE.md에 Docker 셋업, 서비스 라이프사이클, 트러블슈팅 6섹션, 테스트 매트릭스 완성.

### #87. ResponseDispatcher 하드코딩 오프셋
- **등급**: MINOR | **스코프**: gateway | **타입**: bug
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED | **커밋**: df33f60
- **비고**: envelope_payload_offset() 함수 호출로 동적 계산으로 교체.

### #88. 별도 백로그 파일 2건 미이전
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: backlog_memory_os_level.md + 20260315_094300_backlog.md → BACKLOG.md 통합 완료, 원본 삭제.

---
