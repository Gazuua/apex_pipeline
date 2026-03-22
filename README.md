# Apex Pipeline

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 위에 MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL) 를 구축하는 프로젝트.

## Performance — Per-core Architecture

<div align="center">
<img src="docs/apex_core/benchmark/architecture_scaling.png?v=2" alt="Per-core vs Shared io_context Scaling" width="800"/>
</div>

<table>
<tr><td>

**Per-core io_context** 아키텍처가 전통적 Shared 스레드 풀 모델 대비 **워커 수에 비례하는 선형 확장**을 달성합니다.

Shared 모델에는 **64-shard 파티셔닝**(업계 표준 최적화)을 적용했지만, `io_context` 내부 큐 경합으로 처리량이 정체됩니다.

</td></tr>
</table>

<table>
<thead>
<tr>
<th></th>
<th align="center">1워커</th>
<th align="center">2워커</th>
<th align="center">3워커</th>
<th align="center">4워커</th>
</tr>
</thead>
<tbody>
<tr>
<td><b>🟢 Per-core</b></td>
<td align="center">0.51M msg/s</td>
<td align="center"><b>0.90M</b></td>
<td align="center"><b>1.24M</b></td>
<td align="center"><b>1.56M</b></td>
</tr>
<tr>
<td><b>🟡 Shared</b></td>
<td align="center">0.53M</td>
<td align="center">0.64M</td>
<td align="center">0.72M</td>
<td align="center">0.74M</td>
</tr>
<tr>
<td><b>📈 Per-core 배수</b></td>
<td align="center">1.0x</td>
<td align="center"><b>⚡ 1.4x</b></td>
<td align="center"><b>⚡ 1.7x</b></td>
<td align="center"><b>⚡ 2.1x</b></td>
</tr>
</tbody>
</table>

> 📊 [**인터랙티브 벤치마크 보고서**](https://gazuua.github.io/apex_pipeline/benchmark/index.html) · [전체 벤치마크 보고서](https://gazuua.github.io/apex_pipeline/benchmark/report.html)

<details>
<summary>💡 <b>왜 Per-core가 선형 확장하고, Shared는 정체하는가?</b></summary>
<br/>
<table>
<tr>
<td width="50%">

**🟢 Per-core — 선형 확장**

각 워커가 **독립된 `io_context`를 소유**하고, 자기 세션 맵에만 접근합니다.

- Lock이 **존재하지 않음** (lock-free가 아닌 lock-없음)
- 워커 간 공유 상태 **제로** → 캐시 라인 경합 없음
- 워커 추가 시 처리량이 **순증** (1→4워커: 3.06x 확장, 77~88% 효율)

</td>
<td width="50%">

**🟡 Shared — 처리량 정체**

모든 워커가 하나의 `io_context`에서 `run()`을 호출합니다.

- 세션 mutex를 **64샤드로 분산**해도 (업계 표준 최적화)
- `io_context` 내부 **핸들러 큐의 단일 mutex**가 근본 병목
- 워커 4→8→16 늘려도 **0.77M에서 완전 정체**

</td>
</tr>
</table>

**결론:** lock 최적화(샤딩, reader-writer lock 등)만으로는 한계가 있습니다. **`io_context` 자체를 워커별로 분리**해야 진정한 선형 확장이 가능하며, 이것이 Apex Core의 per-core shared-nothing 아키텍처를 채용한 근본적 이유입니다.

<sub>측정 환경: Intel i5-9300H (4C/8T, L3 8MB) · DDR4-2400 · MSVC 19.44 Release · 워크로드: 워커당 1,000 세션 × 50,000 메시지 (세션 조회 + 상태 수정)</sub><br/>
<sub>※ 테스트 PC는 4물리 코어 기준입니다. 물리 코어가 더 많은 프로덕션 서버에서는 4워커 이상에서도 선형 확장이 유지됩니다.</sub>
</details>

## 아키텍처

### Per-core Shared-nothing 모델

Apex Core는 **shared-nothing** 아키텍처를 채택합니다. 각 코어가 자신만의 `io_context`, 세션 맵, 메모리 할당기를 소유하며, 코어 간에 공유되는 상태가 없습니다. Lock이 아예 존재하지 않으므로 lock-free 자료구조의 CAS 오버헤드조차 발생하지 않고, 코어를 추가하면 처리량이 선형으로 증가합니다.

코어 간 통신이 필요한 경우(브로드캐스트, 크로스코어 요청 등)에는 SPSC all-to-all mesh를 사용합니다. 코어 쌍마다 전용 단방향 큐(N×(N-1)개)를 두어 CAS 없이 O(1) enqueue/dequeue를 달성합니다.

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

## 빌드

### Prerequisites

- **C++23 컴파일러**: MSVC 2022 (v19.40+) 또는 GCC 14+
- **CMake** 3.25+, **Ninja**
- **vcpkg** (`VCPKG_ROOT` 환경변수 설정)

### Windows

```bat
build.bat          # Debug 빌드 + 테스트
build.bat release  # Release 빌드
```

### Linux

```bash
./build.sh          # Debug 빌드 + 테스트
./build.sh release  # Release 빌드
```

### CMake Presets

| Preset | 설명 |
|--------|------|
| `debug` | Debug 빌드 (기본) |
| `release` | Release 빌드 |
| `asan` | AddressSanitizer (GCC/Clang) |
| `tsan` | ThreadSanitizer (GCC/Clang) |
| `ubsan` | UndefinedBehaviorSanitizer (GCC/Clang) |
| `iouring` | io_uring 백엔드 (Linux) |

## 인프라

### 개발 환경 (Docker Compose)

Kafka, Redis x4, PostgreSQL, PgBouncer를 로컬에 기동합니다.

```bash
cd apex_infra
docker compose up -d                              # 기본 인프라
docker compose --profile observability up -d       # + Prometheus/Grafana
docker compose down -v                             # 초기화
```

### E2E 테스트 환경

서비스 바이너리 + 전체 인프라를 Docker로 기동하여 End-to-End 테스트를 실행합니다.

```bash
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --wait
```

### 서비스 Dockerfile

| 서비스 | 파일 |
|--------|------|
| Gateway | `apex_infra/docker/gateway.Dockerfile` |
| Auth Service | `apex_infra/docker/auth-svc.Dockerfile` |
| Chat Service | `apex_infra/docker/chat-svc.Dockerfile` |
| CI 빌드 | `apex_infra/docker/ci.Dockerfile` |

## 현재 상태 — v0.5.10.2

### 완료

- **v0.5.10.2 — Session UAF 소멸 순서 수정**
  - Server::~Server()에 명시적 파괴 순서 추가 (listeners → schedulers → core_engine)
  - 미완료 코루틴의 intrusive_ptr\<Session\> UAF 방지

- **v0.5.10.1 — 보안 시크릿 관리 + Blacklist 정책**
  - Redis 4인스턴스 requirepass 적용, PgBouncer 동적 userlist 생성
  - expand_env() apex_shared 추출, TOML ${VAR:-default} 패턴 통일
  - blacklist_fail_open 설정 추가 (기본값 fail-close), GatewayError::BlacklistCheckFailed 도입

- **v0.5.10.0 — SPSC All-to-All Mesh**
  - CoreEngine MPSC inbox → SPSC all-to-all mesh 전환 (N×(N-1) 전용 큐)
  - CAS contention 제거 + cache line bouncing 감소로 크로스코어 레이턴시 개선
  - co_post_to() awaitable API 추가 (backpressure 지원)
  - post_to() 동기 API (SPSC for core threads, asio::post fallback for non-core)
  - BroadcastFanout asio::post 기반 마이그레이션

- **도구: `/fsd-backlog` 백로그 소탕 자동화** — 슬래시 커맨드 한 번으로 백로그 스캔→선별→구현→머지 완전 자율 수행

- **v0.5.9.0 — Tier 3 아키텍처 정비 + 저작권 헤더 + Full Auto-Review**
  - SessionId 강타입화 (`enum class SessionId : uint64_t`)
  - core→shared 역방향 의존 해소 (forwarding header deprecated → 직접 include 전환 완료)
  - ErrorCode 분리, spawn_tracked 도입
  - 전체 소스/스크립트 MIT License 저작권 헤더 추가 (336개 파일)
  - branch-handoff.sh 멀티 에이전트 인수인계 시스템
  - Full Auto-Review v0.5.9.0 (37건 발견: CRITICAL 1 + MAJOR 14 + MINOR 22, 13건 수정)
  - CRITICAL: connection_handler async_write UB 수정 (async_send_raw→enqueue_write_raw)
  - 보안: bcrypt 해시 로그 노출 제거, Docker non-root 실행 전환 (3 서비스)
  - 버전 불일치 통일 (vcpkg.json, CMakeLists.txt → 0.5.9)

- **v0.5.8.1 — BACKLOG 일괄 소탕**
  - CRITICAL 1건 + MAJOR 9건 + MINOR 3건 해결
  - 71/71 유닛 통과 + CI 전체 통과

- **v0.5.8.0 — CI 파이프라인 확장**
  - build matrix 루트 빌드 통합 (apex_core + apex_shared 동시 검증, MSVC 포함)
  - UBSAN CMake preset 추가 (`linux-ubsan`)
  - 서비스 Dockerfile 3개 (Gateway/Auth/Chat) + docker-compose.e2e.yml Docker 기반 서비스 기동
  - CI E2E job: docker compose `--wait` 기동 후 `apex_e2e_tests` 실행
  - Nightly Valgrind workflow: unit + E2E + 스트레스 12개 (cron + workflow_dispatch)
  - 71/71 유닛 테스트 통과 + CI 전체 통과

- **v0.5.7.0 — 코드 위생 확립**
  - `.clang-format` 도입 (Allman brace, 120자, 4칸 인덴트) + 전체 274파일 일괄 포맷팅
  - `.git-blame-ignore-revs` 등록, CI `format-check` job 추가 (clang-format 21.1.8 고정)
  - `apex_set_warnings()` 정의 + 전 타겟 적용 (MSVC `/W4 /WX`, GCC `-Wall -Wextra -Wpedantic -Werror`)
  - 컴파일러 경고 전수 수정 (missing-field-initializers, unused-parameter, redundant-move 등)
  - FileWatcher flaky 테스트 수정 (NTFS 타임스탬프 캐싱 우회)
  - 71/71 유닛 통과 + CI 전체 통과 (GCC, ASAN, TSAN, MSVC)

- **v0.5.6.0 — Post-E2E 코드 리뷰 + 프레임워크 인프라 정비**
  - 10개 관점 체계 리뷰 (46건 발견), ~35건 직접 수정
  - 코어 인프라 확장 D2-D7: server.global\<T\>, wire_services 자동 배선, spawn tracked API, ConsumerPayloadPool
  - ChannelSessionMap per-core shared_mutex 완전 제거, GatewayGlobals 소유권 Server 이관
  - auto-review 5명 (CRITICAL 4 + MAJOR 5 추가 수정, 보안 취약점 1건 포함)
  - 71/71 유닛 통과

- **v0.5.5.2 — 로그 디렉토리 구조 확립**
  - async logger + daily_file_format_sink + exact_level_sink 조합
  - 서비스별/레벨별/날짜별 파일 로깅 구조화
  - 프로젝트 루트 자동 탐지, service_name TOML 설정 + 검증
  - 71/71 유닛 통과

- **v0.5.5.1 — E2E 인프라 수정 + 서비스 체인 검증**
  - 빌드 인프라: BUILD_TESTING=ON (CMakePresets), include(GoogleTest), CTest E2E 제외(-LE e2e)
  - 코어 프레임워크: MessageDispatcher default handler, Server post_init_callback, multi-listener sync_default_handler
  - Gateway: TcpBinaryProtocol listen, TOML 구조 수정, GatewayService 배선(pipeline+router), ResponseDispatcher 정식 배선, 시스템 메시지 처리(AuthenticateSession)
  - Auth/Chat Service: CoreEngine 기반 전환(standalone→정식), 어댑터 init(engine), bcrypt 시드, PG search_path
  - DB 스키마: locked→locked_until, token_family 추가, SQL 컬럼명 수정(id→room_id/message_id)
  - E2E Fixture: 바이너리 경로 주입, working directory, JWT 키 수정, 디버그 로그
  - Server::run() sync_default_handler Phase 3.5 타이밍 수정 (multi-listener handler 동기화)
  - E2E response_topic 정합성 복구 (Auth/Chat → gateway.responses)
  - test_redis_adapter ASAN heap-use-after-free 수정 (소멸 순서 보장)
  - 71/71 유닛 테스트 통과 + 11/11 E2E 통과 (CI ASAN/TSAN 포함 전체 통과)

- **v0.5.5 — 서비스 체인 완성**
  - PR #30 리뷰 이슈 8건 수정 (kafka_envelope overflow 보호, spdlog 의존성 제거, JWT uid claim string 전환, JwtVerifier copy/move 삭제, GatewayPipeline config DI, auth exempt 화이트리스트 TOML, gateway_config_parser exempt 파싱, gateway.toml HS256 제거)
  - Auth Service 완성: MessageDispatcher 기반 핸들러, login/logout/refresh_token (PG+Redis+bcrypt+JWT RS256), EnvelopeMetadata 캐시 패턴, TOML 기반 부트스트랩 main.cpp
  - Chat Service 완성: 8개 핸들러 FBS+PG+Redis (create_room, join, leave, list, send_message, whisper, global_broadcast, chat_history), TOML 기반 부트스트랩 main.cpp
  - E2E 테스트 인프라: RS256 테스트 키, HS256→RS256 테스트 전환, E2E fixture launch/teardown, E2E TOML 설정 3개
  - 56/56 테스트 통과

- **v0.5.4.2 — auto-review 21건 수정 완료**
  - Critical: UAF·바이트오더·JWT 5건 수정
  - Important: 14건 수정
  - Minor: 2건 수정
  - 6건 에스컬레이션 → 백로그 이전
  - 56/56 테스트 통과, CI 통과

- **v0.5.4.1 — Wave 2 패치 (auto-review + 백로그 수정)**
  - Mock 어댑터 인프라 (Kafka, Redis, PG) + Gateway/Auth/Chat 단위 테스트 신설
  - Redis 파라미터 바인딩 API (RedisMultiplexer) + Auth/Gateway 인젝션 방어 마이그레이션
  - Gateway 동시성 수정: ResponseDispatcher Kafka→core post, Rate Limiter per-core 설정 교체
  - Gateway 기능 보강: PubSub WireHeader v2, JWT user_id Kafka 전달, 환경변수 치환, 구독 상한
  - 테스트 품질: sleep 제거 → 시간 주입/poll_now 패턴, 56/56 테스트 통과

- **v0.5.4.0 — 서비스 체인 Wave 2 (Gateway + Auth + Chat + E2E)**
  - Gateway MVP: TLS 종단 (OpenSSL), JWT 검증 (jwt-cpp), msg_id 기반 Kafka 라우팅, TOML hot-reload
  - Rate Limiting 3계층: Per-IP Sliding Window (TimingWheel) / Per-User Redis / Per-Endpoint Config
  - Auth Service: JWT 발급/검증/블랙리스트, bcrypt 해싱, Redis 세션 관리, PostgreSQL 사용자 저장소
  - Chat Service: 방 관리, 메시지/귓속말, 히스토리, PubSub 기반 전역 브로드캐스트
  - E2E 통합 테스트 6개 시나리오 (인증 → 채팅 → 브로드캐스트 전체 경로)

- **v0.5.0.0 — Protocol concept 기반 의존성 역전 + 어댑터 회복력 + WebSocket MVP**
  - Protocol concept 기반 의존성 역전: core=concept 정의, shared=구현 (TcpBinaryProtocol, WebSocketProtocol)
  - Server 비템플릿 리팩터링 + `listen<P>(port)` 멀티 프로토콜 지원
  - ListenerBase virtual (lifecycle) + ConnectionHandler\<P\> (zero-overhead I/O)
  - per-session write queue (std::deque + write_pump 코루틴)
  - CircuitBreaker (plain class, composition) + AdapterState 상태 머신
  - Redis AUTH/ARRAY 응답 파싱, 어댑터 retry/reconnect, DLQ (Dead Letter Queue)
  - 공유 프로토콜 스키마 4종 (apex_shared/lib/protocols/)
  - 단위 테스트 48 → 51 (신규 28 TC), auto-review 4라운드 Clean

- **v0.4.5.2 — auto-review 감도 강화 + 코드 리뷰 이슈 수정 + 리뷰 피드백 반영**
  - auto-review 감도 강화 (체크리스트, threshold 50%, cross-domain 관심사) + cross-cutting 리뷰어 신설 → 12명 체제
  - 코드 리뷰 이슈 6건 수정: PendingCommand UAF, silent disconnect 로깅, 어댑터 init 실패 throw, RingBuffer shrink 등
  - 리뷰 피드백 반영: SessionManager::tick() shrink_to_fit 60초 주기, RedisMultiplexer 주석 보강, 백로그 정리, clangd 경로 수정

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

- **auto-review v2.0 — 3계층 팀 구조 개편** (도구/문서 개선, 프레임워크 버전 변경 없음)
  - auto-review 플러그인 구조 정립: auto-review.md 오케스트레이터 → 7 reviewer agents
  - 리뷰어 자율성 원칙 명시 (12 에이전트 파일 갱신)
  - full mode 리뷰 이슈 41건 수정 (코드 5건 + docs/README 5건 + docs 경로/체크박스 31건)
  - CLAUDE.md 빌드 명령어 + 에이전트 작업 규칙 추가
  - v0.5 백로그 문서 작성

- **v0.4.5 — 코어 메모리 아키텍처 + 어댑터 개선**
  - 코어 메모리 아키텍처: BumpAllocator (요청 수명) + ArenaAllocator (트랜잭션 수명) + SlabPool→SlabAllocator 리네임
  - C++20 concepts: CoreAllocator/Freeable/Resettable concept, PoolLike concept (CRTP ConnectionPool 제거)
  - Redis: RedisPool→RedisMultiplexer (코어당 고정 커넥션 + 코루틴 파이프라이닝), RedisReply wrapper
  - PG: PgTransaction RAII 가드, PgConnection prepared statement + BumpAllocator 주입
  - CoreMetrics atomic 카운터 + rate-limited 로깅, TOML 설정 스키마 확장
  - 45 단위 테스트 + 4 통합 테스트, Auto-review 2 rounds Clean

- **v0.4 — 외부 어댑터** (PR #15 merged)
  - 공통 추상화: AdapterBase CRTP + PoolLike concept + AdapterInterface 타입 소거
  - Kafka 어댑터: librdkafka Producer/Consumer + Asio 통합 + KafkaSink (spdlog → Kafka)
  - Redis 어댑터: hiredis fd → Asio 직접 등록 (HiredisAsioAdapter) + 코루틴 브릿지
  - PostgreSQL 어댑터: libpq async → Asio + PgPool lazy connect + PgBouncer 전제
  - Server 통합: add_adapter API + Graceful Shutdown 순서 보장
  - 통합 테스트 인프라: docker-compose (Kafka/Redis/PG/PgBouncer) + CMake option

- **v0.3 — 코어 성능 완성** (PR #10 merged)
  - Tier 0: Google Benchmark 벤치마크 인프라 (micro 7개 + integration 4개)
  - Tier 0.5: 에러 타입 통일 (DispatchError/QueueError → ErrorCode 단일 채널, Result<T> 도입)
  - Tier 1: drain/tick 분리, Cross-Core Message Passing 인프라, MessageDispatcher unordered_flat_map, zero-copy dispatch
  - Tier 1.5: E2E echo 부하 테스터 + JSON before/after 비교 도구
  - Tier 2: intrusive_ptr 전환, Session SlabPool 할당, sessions_/timer_to_session_ unordered_flat_map
  - Tier 2.5: 벤치마크 시각화 (matplotlib) + PDF 보고서 (ReportLab) 파이프라인
  - Tier 3: Server/ConnectionHandler 분리, SO_REUSEPORT per-core acceptor, io_uring CMake 옵션, SlabPool auto-grow

- **v0.2 — 개발 인프라** (PR #1 merged)
  - TOML 설정 시스템 (tomlplusplus)
  - spdlog 구조화 로깅
  - Graceful shutdown (시그널 핸들링 + drain 타임아웃)
  - GitHub Actions CI (GCC debug/asan/tsan + MSVC, 5개 잡 병렬)
  - 빌드 환경 개선 (.gitattributes, Docker CI, 사전 체크 공용 헬퍼)

- **v0.1 — 코어 프레임워크 기초**
  - Boost.Asio 코루틴 기반 TCP 서버/클라이언트
  - 커스텀 프레임 프로토콜 (Length-prefixed binary frame)
  - CoreEngine 멀티코어 아키텍처 (코어별 독립 io_context + 크로스코어 메시징)
  - FlatBuffers 직렬화, RingBuffer, MessageDispatcher
  - 서비스 레지스트리 + 파이프라인 체인
