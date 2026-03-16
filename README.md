# Apex Pipeline

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 위에 MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL) 를 구축하는 프로젝트.

## 현재 상태 — v0.5.5.1

### 완료

- **v0.1 — 코어 프레임워크 기초**
  - Boost.Asio 코루틴 기반 TCP 서버/클라이언트
  - 커스텀 프레임 프로토콜 (Length-prefixed binary frame)
  - CoreEngine 멀티코어 아키텍처 (코어별 독립 io_context + 크로스코어 메시징)
  - FlatBuffers 직렬화, RingBuffer, MessageDispatcher
  - 서비스 레지스트리 + 파이프라인 체인

- **v0.2 — 개발 인프라** (PR #1 merged)
  - TOML 설정 시스템 (tomlplusplus)
  - spdlog 구조화 로깅
  - Graceful shutdown (시그널 핸들링 + drain 타임아웃)
  - GitHub Actions CI (GCC debug/asan/tsan + MSVC, 5개 잡 병렬)
  - 빌드 환경 개선 (.gitattributes, Docker CI, 사전 체크 공용 헬퍼)

- **v0.3 — 코어 성능 완성** (PR #10 merged)
  - Tier 0: Google Benchmark 벤치마크 인프라 (micro 7개 + integration 4개)
  - Tier 0.5: 에러 타입 통일 (DispatchError/QueueError → ErrorCode 단일 채널, Result<T> 도입)
  - Tier 1: drain/tick 분리, Cross-Core Message Passing 인프라, MessageDispatcher unordered_flat_map, zero-copy dispatch
  - Tier 1.5: E2E echo 부하 테스터 + JSON before/after 비교 도구
  - Tier 2: intrusive_ptr 전환, Session SlabPool 할당, sessions_/timer_to_session_ unordered_flat_map
  - Tier 2.5: 벤치마크 시각화 (matplotlib) + PDF 보고서 (ReportLab) 파이프라인
  - Tier 3: Server/ConnectionHandler 분리, SO_REUSEPORT per-core acceptor, io_uring CMake 옵션, SlabPool auto-grow

- **v0.4 — 외부 어댑터** (PR #15 merged)
  - 공통 추상화: AdapterBase CRTP + PoolLike concept + AdapterInterface 타입 소거
  - Kafka 어댑터: librdkafka Producer/Consumer + Asio 통합 + KafkaSink (spdlog → Kafka)
  - Redis 어댑터: hiredis fd → Asio 직접 등록 (HiredisAsioAdapter) + 코루틴 브릿지
  - PostgreSQL 어댑터: libpq async → Asio + PgPool lazy connect + PgBouncer 전제
  - Server 통합: add_adapter API + Graceful Shutdown 순서 보장
  - 통합 테스트 인프라: docker-compose (Kafka/Redis/PG/PgBouncer) + CMake option

- **v0.4.5 — 코어 메모리 아키텍처 + 어댑터 개선**
  - 코어 메모리 아키텍처: BumpAllocator (요청 수명) + ArenaAllocator (트랜잭션 수명) + SlabPool→SlabAllocator 리네임
  - C++20 concepts: CoreAllocator/Freeable/Resettable concept, PoolLike concept (CRTP ConnectionPool 제거)
  - Redis: RedisPool→RedisMultiplexer (코어당 고정 커넥션 + 코루틴 파이프라이닝), RedisReply wrapper
  - PG: PgTransaction RAII 가드, PgConnection prepared statement + BumpAllocator 주입
  - CoreMetrics atomic 카운터 + rate-limited 로깅, TOML 설정 스키마 확장
  - 45 단위 테스트 + 4 통합 테스트, Auto-review 2 rounds Clean

- **auto-review v2.0 — 3계층 팀 구조 개편** (도구/문서 개선, 프레임워크 버전 변경 없음)
  - auto-review 플러그인 구조 정립: auto-review.md 오케스트레이터 → 7 reviewer agents
  - 리뷰어 자율성 원칙 명시 (12 에이전트 파일 갱신)
  - full mode 리뷰 이슈 41건 수정 (코드 5건 + docs/README 5건 + docs 경로/체크박스 31건)
  - CLAUDE.md 빌드 명령어 + 에이전트 작업 규칙 추가
  - v0.5 백로그 문서 작성

- **v0.4.5.1 — Full auto-review + 문서 정비 + 프로세스 개선**
  - Full auto-review v0.4.5.0: 11명 리뷰어, 코드 4건 수정
  - 문서 타임스탬프 전수 보정 (93건)
  - BACKLOG.md 신설 + 원본 문서 TODO/백로그 전수 제거
  - CLAUDE.md 압축 분할 (116줄 → 54줄, 상세는 하위 CLAUDE.md로 분리)
  - auto-review re_review_scope 기반 스마트 재리뷰 판단 도입
  - 빌드 무한대기 규칙 + 타임스탬프 date 명령 강제 지침
  - CI Linux vcpkg binary cache 경로 수정
  - Full auto-review v0.4.5.1: Round 1 Clean, 문서 수정 2건
  - 프로세스 개선 3건: start 시그널 타이밍 해소, 빌드 역할 분리, 팀 해산 책임 명확화

- **v0.4.5.2 — auto-review 감도 강화 + 코드 리뷰 이슈 수정 + 리뷰 피드백 반영**
  - auto-review 감도 강화 (체크리스트, threshold 50%, cross-domain 관심사) + cross-cutting 리뷰어 신설 → 12명 체제
  - 코드 리뷰 이슈 6건 수정: PendingCommand UAF, silent disconnect 로깅, 어댑터 init 실패 throw, RingBuffer shrink 등
  - 리뷰 피드백 반영: SessionManager::tick() shrink_to_fit 60초 주기, RedisMultiplexer 주석 보강, 백로그 정리, clangd 경로 수정

- **v0.5.0.0 — Protocol concept 기반 의존성 역전 + 어댑터 회복력 + WebSocket MVP**
  - Protocol concept 기반 의존성 역전: core=concept 정의, shared=구현 (TcpBinaryProtocol, WebSocketProtocol)
  - Server 비템플릿 리팩터링 + `listen<P>(port)` 멀티 프로토콜 지원
  - ListenerBase virtual (lifecycle) + ConnectionHandler\<P\> (zero-overhead I/O)
  - per-session write queue (std::deque + write_pump 코루틴)
  - CircuitBreaker (plain class, composition) + AdapterState 상태 머신
  - Redis AUTH/ARRAY 응답 파싱, 어댑터 retry/reconnect, DLQ (Dead Letter Queue)
  - 공유 프로토콜 스키마 4종 (apex_shared/lib/protocols/)
  - 단위 테스트 48 → 51 (신규 28 TC), auto-review 4라운드 Clean

- **v0.5.4.0 — 서비스 체인 Wave 2 (Gateway + Auth + Chat + E2E)**
  - Gateway MVP: TLS 종단 (OpenSSL), JWT 검증 (jwt-cpp), msg_id 기반 Kafka 라우팅, TOML hot-reload
  - Rate Limiting 3계층: Per-IP Sliding Window (TimingWheel) / Per-User Redis / Per-Endpoint Config
  - Auth Service: JWT 발급/검증/블랙리스트, bcrypt 해싱, Redis 세션 관리, PostgreSQL 사용자 저장소
  - Chat Service: 방 관리, 메시지/귓속말, 히스토리, PubSub 기반 전역 브로드캐스트
  - E2E 통합 테스트 6개 시나리오 (인증 → 채팅 → 브로드캐스트 전체 경로)

- **v0.5.4.1 — Wave 2 패치 (auto-review + 백로그 수정)**
  - Mock 어댑터 인프라 (Kafka, Redis, PG) + Gateway/Auth/Chat 단위 테스트 신설
  - Redis 파라미터 바인딩 API (RedisMultiplexer) + Auth/Gateway 인젝션 방어 마이그레이션
  - Gateway 동시성 수정: ResponseDispatcher Kafka→core post, Rate Limiter per-core 설정 교체
  - Gateway 기능 보강: PubSub WireHeader v2, JWT user_id Kafka 전달, 환경변수 치환, 구독 상한
  - 테스트 품질: sleep 제거 → 시간 주입/poll_now 패턴, 56/56 테스트 통과

- **v0.5.4.2 — auto-review 21건 수정 완료**
  - Critical: UAF·바이트오더·JWT 5건 수정
  - Important: 14건 수정
  - Minor: 2건 수정
  - 6건 에스컬레이션 → 백로그 이전
  - 56/56 테스트 통과, CI 통과

- **v0.5.5 — 서비스 체인 완성**
  - PR #30 리뷰 이슈 8건 수정 (kafka_envelope overflow 보호, spdlog 의존성 제거, JWT uid claim string 전환, JwtVerifier copy/move 삭제, GatewayPipeline config DI, auth exempt 화이트리스트 TOML, gateway_config_parser exempt 파싱, gateway.toml HS256 제거)
  - Auth Service 완성: MessageDispatcher 기반 핸들러, login/logout/refresh_token (PG+Redis+bcrypt+JWT RS256), EnvelopeMetadata 캐시 패턴, TOML 기반 부트스트랩 main.cpp
  - Chat Service 완성: 8개 핸들러 FBS+PG+Redis (create_room, join, leave, list, send_message, whisper, global_broadcast, chat_history), TOML 기반 부트스트랩 main.cpp
  - E2E 테스트 인프라: RS256 테스트 키, HS256→RS256 테스트 전환, E2E fixture launch/teardown, E2E TOML 설정 3개
  - 56/56 테스트 통과

- **v0.5.5.1 — E2E 인프라 수정 + 서비스 체인 검증**
  - 빌드 인프라: BUILD_TESTING=ON (CMakePresets), include(GoogleTest), CTest E2E 제외(-LE e2e)
  - 코어 프레임워크: MessageDispatcher default handler, Server post_init_callback, multi-listener sync_default_handler
  - Gateway: TcpBinaryProtocol listen, TOML 구조 수정, GatewayService 배선(pipeline+router), ResponseDispatcher 정식 배선, 시스템 메시지 처리(AuthenticateSession)
  - Auth/Chat Service: CoreEngine 기반 전환(standalone→정식), 어댑터 init(engine), bcrypt 시드, PG search_path
  - DB 스키마: locked→locked_until, token_family 추가, SQL 컬럼명 수정(id→room_id/message_id)
  - E2E Fixture: 바이너리 경로 주입, working directory, JWT 키 수정, 디버그 로그
  - 67/67 유닛 테스트 통과 + E2E LoginAndAuthenticatedRequest 통과

## 아키텍처

### Per-core 싱글 스레드 모델

각 코어가 독립 `io_context`에서 실행되는 락 프리 설계. 코어 간 통신은 `cross_core_post_msg`(MPSC 큐 기반)로 안전하게 처리.

### 3계층 메모리 아키텍처

| 계층 | 구현 | 역할 |
|------|------|------|
| L1 로컬 캐시 | CoreAllocator (BumpAllocator / ArenaAllocator / SlabAllocator) | per-core 메모리 관리, 락 프리 |
| L2 | Redis (RedisMultiplexer) | 코어 간 공유 캐시 |
| L3 | PostgreSQL (PgPool) | 영속 저장소 |

- **BumpAllocator**: 요청 수명 — 요청 처리 후 한 번에 해제
- **ArenaAllocator**: 트랜잭션 수명 — 트랜잭션 단위 할당/해제
- **SlabAllocator**: 객체 풀 — Session 등 고정 크기 객체 재사용

### SharedPayload

cross-core 메시지 공유를 위한 atomic refcount 기반 zero-copy 구조체. 코어 간 메시지 전달 시 데이터 복사 없이 소유권만 이전.

### 다음

- **v0.6 — 운영 인프라** (Prometheus + Docker + K8s + CI/CD)
- **v1.0.0.0 — 프레임워크 완성**

> 상세 로드맵: `docs/Apex_Pipeline.md` §10
