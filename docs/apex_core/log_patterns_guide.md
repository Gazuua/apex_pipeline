# 로그 패턴 가이드

**버전**: v0.6.3.0 | **최종 갱신**: 2026-03-25
**목적**: 서비스 기동·운영 시 발생하는 로그 패턴의 정상/비정상 판별 기준과 트러블슈팅 가이드. 에이전트와 사람 모두를 위한 참조 문서.

---

## 1. 로그 아키텍처 개요

### 이중 로거 체계

| 로거 이름 | 용도 | 레벨 설정 | 사용 영역 |
|-----------|------|-----------|-----------|
| `"apex"` | 프레임워크 내부 | `framework_level` | apex_core (Server, CoreEngine, Session 등) |
| `"app"` | 서비스 비즈니스 로직 | `level` | apex_shared 어댑터, 서비스 (Gateway, Auth, Chat) |

두 로거가 **독립된 레벨**로 운영된다. 프레임워크를 `trace`로 놓고 앱은 `info`만 볼 수 있어, 디버깅 시 필요한 레이어만 확대 가능.

### 출력 포맷

**콘솔 (spdlog 패턴)**:
```
%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v
│                      │     │    └─ ScopedLogger가 조립한 메시지
│                      │     └────── 로거 이름: "apex" / "app"
│                      └──────────── 로그 레벨
└─────────────────────────────────── 타임스탬프 (밀리초)
```

**파일** — 레벨별 6파일 분리 (`logs/{service_name}/YYYYMMDD_{level}.log`), JSON 포맷 선택 가능:
```json
{"ts":"2026-03-24T09:00:01.130","level":"info","logger":"apex","msg":"[server.cpp:148 Server::run] [Server] starting with 4 cores"}
```

### ScopedLogger 메시지 구조

ScopedLogger가 조립하는 `%v` 부분의 구조:

```
[file:line Class::Method] [core=N][Component][sess=N][msg=0xHHHH][trace=hex] message
```

태그는 컨텍스트에 따라 선택적으로 출현한다. 상세는 §2 태그 레퍼런스 참조.

---

## 2. 태그 레퍼런스

| 태그 | 의미 | 포맷 | 출현 조건 |
|------|------|------|-----------|
| `[file:line Func]` | 소스 위치 (경로 제거, `Class::Method`만) | `[server.cpp:148 Server::run]` | **항상** |
| `[core=N]` | 코어 ID (0부터) | `[core=2]` | `core_id != NO_CORE` (per-core 컴포넌트) |
| `[Component]` | 컴포넌트명 | `[SessionManager]` | **항상** |
| `[sess=N]` | 세션 ID (코어별 단조 증가 uint64) | `[sess=42]` | SessionId 오버로드 사용 시 |
| `[msg=0xHHHH]` | 메시지 ID (와이어 프로토콜) | `[msg=0x03E9]` | msg_id 오버로드 사용 시 |
| `[trace=hex]` | 요청 추적 correlation ID | `[trace=a1b2c3d4]` | `with_trace()` 사용 시 |

**grep 활용 팁**:
- 특정 세션 추적: `grep "\[sess=42\]"`
- 특정 컴포넌트: `grep "\[SessionManager\]"`
- 특정 코어: `grep "\[core=2\]"`
- 요청 추적: `grep "\[trace=a1b2c3d4\]"`

---

## 3. 정상 패턴 카탈로그

### 3.1 기동 시퀀스

서비스 기동 시 **반드시 이 순서로** 로그가 출현해야 정상:

```log
[info] [apex] [config.cpp:183 AppConfig::from_file] [Config] loading config from 'config.toml'
[info] [apex] [config.cpp:198 AppConfig::from_file] [Config] config loaded: cores=4, log_level=info
[info] [apex] [server.cpp:148 Server::run] [Server] starting with 4 cores
[debug] [apex] [server.cpp:155 Server::run] [Server] adapters initialized (2)
[debug] [apex] [server.cpp:183 Server::run] [Server] Phase 1: on_configure
[debug] [apex] [server.cpp:200 Server::run] [Server] Phase 2: on_wire
[debug] [apex] [server.cpp:225 Server::run] [Server] Phase 3: on_start
[info] [apex] [tcp_acceptor.cpp:49 TcpAcceptor::bind] [core=0][TcpAcceptor] bound to 0.0.0.0:9000
[debug] [apex] [server.cpp:286 Server::run] [Server] starting CoreEngine
[info] [apex] [server.cpp:298 Server::run] [Server] ready — {N} cores, {N} listeners, {N} adapters
```

**확인 포인트**: `ready —` 로그가 보이면 서비스가 완전히 초기화되어 연결을 수락하는 상태.

### 3.2 세션 생명주기

```log
[info] [apex] [SessionManager::create] [core=2][SessionManager] session created id=42 total=5
[trace] [apex] [Session::enqueue_write] [core=2][Session] enqueue_write id=42 size=128 depth=1
[debug] [apex] [Session::close] [core=2][Session] close id=42 prev_state=1
[debug] [apex] [SessionManager::remove] [core=2][SessionManager] session removing id=42
```

정상 종료 경로: `close prev_state=1` (Active → Closing).

타임아웃 경로:
```log
[info] [apex] [SessionManager::check_timeout] [core=2][SessionManager] session timeout id=42
```

### 3.3 메시지 디스패치

```log
[trace] [apex] [FrameCodec::try_decode] [core=2][FrameCodec] try_decode ok msg_id=1001 body=64
[trace] [apex] [CoreEngine::dispatch_message] [core=2][CoreEngine] dispatch_message core=2 op=1001 from_core=2
```

### 3.4 Cross-Core 통신

```log
[trace] [apex] [CoreEngine::post_to] [core=0][CoreEngine] post_to src=0 dst=2 op=3
[debug] [apex] [CrossCoreDispatcher::register_handler] [core=0][CrossCoreDispatcher] register_handler op=3
[trace] [apex] [CrossCoreDispatcher::dispatch] [core=2][CrossCoreDispatcher] dispatch core=2 src=0 op=3
```

### 3.5 주기적 태스크

```log
[info] [apex] [PeriodicTaskScheduler::schedule] [core=0][PeriodicTaskScheduler] schedule handle=1 interval=5000ms
[trace] [apex] [PeriodicTaskScheduler::execute] [core=0][PeriodicTaskScheduler] execute handle=1
```

### 3.6 셧다운 시퀀스

```log
[info] [apex] [Server::stop] [Server] shutdown initiated, drain_timeout=30s
[info] [apex] [Server::poll_shutdown] [Server] All sessions drained, shutting down
[info] [apex] [PeriodicTaskScheduler::stop_all] [core=0][PeriodicTaskScheduler] stop_all count=2
[info] [apex] [Server::shutdown_impl] [Server] shutdown step 5: closing adapters
[info] [apex] [Server::shutdown_impl] [Server] shutdown step 6: stopping core engine
```

---

## 4. 비정상 패턴 카탈로그

모든 warn/error/critical 로그를 모듈별로 분류. 각 패턴에 대해 **의미**와 **조치 방향**을 명시한다.

### 4.1 apex_core (프레임워크)

#### critical — 서비스 기동/운영 불가

| 메시지 | 컴포넌트 | 소스 | 의미 | 조치 |
|--------|----------|------|------|------|
| `invalid bind_address '{addr}' — {err}` | TcpAcceptor | tcp_acceptor.cpp:31 | 리스너 바인드 실패 | config의 bind_address/port 확인, 포트 충돌 확인 |

#### error — 기능 장애

| 메시지 | 컴포넌트 | 소스 | 의미 | 조치 |
|--------|----------|------|------|------|
| `cross-core task exception core={}: {}` | CoreEngine | core_engine.cpp:263 | 코어 간 태스크 실행 중 예외 | 예외 메시지의 원인 추적, 해당 op 핸들러 확인 |
| `cross-core task unknown exception core={}` | CoreEngine | core_engine.cpp:267 | 코어 간 태스크 미지 예외 | 비표준 예외 throw 지점 확인 |
| `tick_callback exception core={}: {}` | CoreEngine | core_engine.cpp:355 | 틱 콜백(타이밍 휠) 예외 | SessionManager::tick() 내부 확인 |
| `tick_callback unknown exception core={}` | CoreEngine | core_engine.cpp:359 | 틱 콜백 미지 예외 | 동일 |
| `cross-core task exception core={}: {}` | SpscMesh | spsc_mesh.cpp:103 | SPSC 큐 태스크 실행 예외 | 동일 패턴, CoreEngine 경유와 별도 경로 |
| `cross-core task unknown exception core={}` | SpscMesh | spsc_mesh.cpp:107 | SPSC 큐 태스크 미지 예외 | 동일 |
| `default handler for msg_id 0x{:08x} threw: {}` | MessageDispatcher | message_dispatcher.cpp:44 | 기본 핸들러 예외 | 서비스의 default handler 구현 확인 |
| `handler for msg_id 0x{:08x} threw: {}` | MessageDispatcher | message_dispatcher.cpp:62 | 메시지 핸들러 예외 | 해당 msg_id 핸들러 구현 확인 |
| `timeout callback exception for session {}: {}` | SessionManager | session_manager.cpp:180 | 세션 타임아웃 콜백 예외 | on_session_closed 구현 확인 |
| `timeout callback unknown exception for session {}` | SessionManager | session_manager.cpp:184 | 세션 타임아웃 콜백 미지 예외 | 동일 |

#### warn — 주의 (즉시 장애 아님, 조치 필요)

| 메시지 | 컴포넌트 | 소스 | 의미 | 조치 |
|--------|----------|------|------|------|
| `post_to src={} dst={} failed: SPSC queue full` | CoreEngine | core_engine.cpp:176 | SPSC 큐 포화, 대상 코어 처리 지연 | 코어별 부하 분산 확인, 큐 크기 조정 |
| `session SlabAllocator exhausted, heap fallback` | SessionManager | session_manager.cpp:32 | 슬랩 풀 고갈, 힙 폴백 | 동시 세션 수 급증 의심, 풀 크기 조정 |
| `enqueue_write id={} queue full depth={}/{}` | Session | session.cpp:120 | 쓰기 큐 포화, 클라이언트 수신 지연 | 클라이언트 네트워크 상태, 메시지 크기 확인 |
| `enqueue_and_await id={} queue full depth={}/{}` | Session | session.cpp:220 | 동기 쓰기 큐 포화 | 동일 |
| `close id={} socket error: {}` | Session | session.cpp:107 | 소켓 종료 시 에러 | 대부분 이미 닫힌 소켓, 반복 시 네트워크 확인 |
| `write_pump id={} write error: {}` | Session | session.cpp:173 | 쓰기 펌프 에러 | 연결 끊김 또는 네트워크 이상 |
| `double-free detected count={}` | SlabAllocator | slab_allocator.cpp:174 | 슬랩 이중 해제 — **수명 관리 버그** | 세션 해제 경로 확인, SessionPtr 수명 추적 |
| `write overflow requested={} writable={}` | RingBuffer | ring_buffer.cpp:137 | 링 버퍼 쓰기 초과 | 버퍼 크기 vs 쓰기량 확인 |
| `dispatch core={} src={} op={} — no handler` | CrossCoreDispatcher | cross_core_dispatcher.cpp:38 | 미등록 cross-core op | 핸들러 등록 누락 확인 |
| `created with zero capacity` | BumpAllocator | bump_allocator.cpp:16 | 버프 할당기 0 용량 | 설정 오류 |
| `Destructor called while still running` | Server | server.cpp:80 | 서버 구동 중 소멸자 호출 | stop() 호출 누락 확인 |
| `Drain timeout expired, {} sessions remaining` | Server | server.cpp:367 | 셧다운 시 세션 드레인 타임아웃 | drain_timeout 조정 또는 세션 미해제 원인 확인 |

### 4.2 apex_shared (어댑터 인프라)

#### Redis

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| error | `host is empty — aborting adapter init` | redis_adapter.cpp:25 | Redis 호스트 미설정 | config 확인 |
| error | `port is 0 — aborting adapter init` | redis_adapter.cpp:30 | Redis 포트 미설정 | config 확인 |
| warn | `initial connect failed, starting reconnect` | redis_multiplexer.cpp:46 | 초기 연결 실패, 재연결 시도 | Redis 서버 상태/네트워크 확인 |
| warn | `AUTH failed` | redis_multiplexer.cpp:325 | Redis 인증 실패 | 비밀번호 확인 |

#### PostgreSQL

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| error | `connection_string is empty` | pg_adapter.cpp:29 | PG 연결 문자열 미설정 | config 확인 |
| error | `PQconnectStart failed: {}` | pg_connection.cpp:148 | PG 연결 시작 실패 | 연결 문자열/네트워크 확인 |
| error | `PQsendQuery failed: {}` | pg_connection.cpp:240 | 쿼리 전송 실패 | 연결 상태/쿼리 문법 확인 |
| error | `destroyed without commit/rollback — poisoning` | pg_transaction.hpp:43 | **트랜잭션 누수** | 트랜잭션 수명 관리 확인 |

#### Kafka

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| error | `init failed: {}` | kafka_producer.cpp:113 | 프로듀서 초기화 실패 | 브로커 주소/네트워크 확인 |
| error | `subscribe failed: {}` | kafka_consumer.cpp:141 | 토픽 구독 실패 | 토픽 존재 여부, 권한 확인 |
| warn | `produce failed: topic={}, err={}` | kafka_producer.cpp:148 | 메시지 발행 실패 | 브로커 상태 확인 |
| warn | `delivery failed: topic={}, err={}` | kafka_producer.cpp:189 | 전달 확인 실패 | 브로커 상태/네트워크 확인 |
| warn | `Message processing failed (topic={}, offset={})` | kafka_consumer.cpp:227 | 메시지 처리 실패 | 서비스 핸들러 확인 |
| error | `DLQ produce failed` | kafka_consumer.cpp:247 | DLQ 발행 실패 | DLQ 토픽/브로커 확인 |
| warn | `auto-wired dispatch failed: {}` | kafka_adapter.cpp:186 | 자동 배선 디스패치 실패 | 서비스 핸들러 등록 확인 |

#### CircuitBreaker

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| warn | `state CLOSED->OPEN failures={}/{}` | circuit_breaker.cpp:96 | **서킷 오픈** — 외부 서비스 연속 실패 | 대상 서비스 상태 확인, failure_threshold 조정 |
| warn | `state HALF_OPEN->OPEN (probe failed)` | circuit_breaker.cpp:102 | 반개방 프로브 실패, 서킷 재오픈 | 대상 서비스가 아직 복구되지 않음 |

#### TLS/TCP

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| warn | `TLS accept failed: {}` | tls_tcp_transport.hpp:73 | TLS 수락 실패 | 인증서/키 설정, 클라이언트 호환성 확인 |
| warn | `TLS handshake failed: {}` | tls_tcp_transport.hpp:86 | TLS 핸드셰이크 실패 | 동일 |

### 4.3 Gateway 서비스

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| error | `RS256 public key not loaded from '{}'` | jwt_verifier.cpp:46 | JWT 공개키 로드 실패 — **인증 불가** | 키 파일 경로/형식 확인 |
| error | `Failed to parse updated config` | config_reloader.cpp:53 | 핫 리로드 config 파싱 실패 | config 문법 확인 (이전 설정 유지) |
| error | `handle_request: route failed for msg_id={}` | gateway_service.cpp:432 | 라우팅 실패 | 라우트 테이블에 해당 msg_id 등록 확인 |
| warn | `No route for msg_id: {}` | message_router.cpp:31 | 미등록 msg_id 라우팅 시도 | 클라이언트가 잘못된 msg_id 전송 또는 라우트 누락 |
| warn | `PendingRequestsMap full ({}/{})` | pending_requests.cpp:21 | 대기 요청 맵 포화 | 백엔드 응답 지연 의심, 맵 크기 조정 |
| warn | `authenticate: blacklist check failed (Redis error)` | gateway_pipeline.hpp:156 | JWT 블랙리스트 Redis 오류 | Redis 연결 상태 확인 |
| warn | `PubSub connect failed: {}` | pubsub_listener.cpp:119 | Redis PubSub 연결 실패 | Redis 상태 확인 |

### 4.4 Auth 서비스

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| error | `Cannot create token — private key not loaded` | jwt_manager.cpp:76 | JWT 서명 불가 — **인증 발급 중단** | RSA 키 파일 확인 |
| error | `Cannot create token — JTI generation failed (CSPRNG)` | jwt_manager.cpp:88 | CSPRNG 실패 — 시스템 엔트로피 문제 | OS 엔트로피 소스 확인 |
| error | `PG query failed for login: {}` | auth_service.cpp:137 | 로그인 PG 쿼리 실패 | DB 연결 상태/쿼리 확인 |
| warn | `Login failed: user not found` | auth_service.cpp:144 | 존재하지 않는 사용자 | 정상적 실패 (공격 탐지 시 빈도 모니터링) |
| warn | `Login failed: account locked` | auth_service.cpp:170 | 계정 잠김 | 잠금 정책 확인 |
| warn | `Login failed: bad credentials` | auth_service.cpp:184 | 비밀번호 불일치 | 정상적 실패 (브루트포스 시 빈도 모니터링) |
| warn | `Refresh token reuse detected!` | auth_service.cpp:388 | **토큰 재사용 탐지** — 토큰 탈취 의심 | 보안 이벤트, 해당 사용자 세션 전체 폐기 확인 |

### 4.5 Chat 서비스

| Lv | 메시지 | 소스 | 의미 | 조치 |
|----|--------|------|------|------|
| error | `PG INSERT chat_rooms failed: {}` | chat_service.cpp:214 | 채팅방 생성 PG 실패 | DB 상태 확인 |
| error | `Kafka persist produce failed for room {}: {}` | chat_service.cpp:540 | 메시지 영속화 Kafka 실패 | Kafka 상태 확인 |
| error | `ChatMessage verification failed` | chat_db_consumer.cpp:26 | DB 컨슈머 메시지 검증 실패 | 메시지 무결성 확인 |
| warn | `Whisper to self rejected` | chat_service.cpp:577 | 자기 자신에게 귓속말 시도 | 클라이언트 로직 확인 |

---

## 5. 트러블슈팅 체크리스트

증상별 진단 플로우. 각 단계에서 grep 패턴과 확인할 코드를 명시한다.

### 서비스가 기동되지 않음

```
1. grep "critical" → bind 실패 / 치명적 초기화 오류
2. grep "invalid bind_address" → 포트 충돌 / 잘못된 주소
3. grep "config" + "error" → config 파싱 실패
4. grep "aborting adapter init" → 어댑터 설정 누락 (Redis host, PG connection_string)
5. "ready —" 로그 부재 확인 → Phase 1/2/3 중 어디서 멈췄는지 특정
```

### 세션이 예기치 않게 끊김

```
1. grep "[sess=N]" → 해당 세션의 전체 생명주기 추적
2. grep "session timeout" → 하트비트 타임아웃 (heartbeat_timeout 설정 확인)
3. grep "write error" → 네트워크 단절
4. grep "queue full" → 쓰기 큐 포화 (클라이언트 수신 지연)
5. grep "close.*socket error" → 소켓 상태 이상
```

### 메시지가 처리되지 않음

```
1. grep "try_decode" → 프레임 디코딩 성공 여부
2. grep "parse.*failed" → 와이어 헤더 파싱 실패 (프로토콜 불일치)
3. grep "no handler" → 핸들러 미등록 (CrossCoreDispatcher)
4. grep "No route" → 라우트 테이블에 msg_id 누락 (Gateway)
5. grep "handler.*threw" → 핸들러 예외 (MessageDispatcher)
```

### 메모리 사용량 증가

```
1. grep "heap fallback" → SlabAllocator 풀 고갈 (세션 급증)
2. grep "double-free" → 수명 관리 버그 (SessionPtr 추적)
3. grep "write overflow" → RingBuffer 초과 쓰기
4. grep "zero capacity" → BumpAllocator 0 용량 설정 오류
```

### 외부 서비스 연결 불안정

```
1. grep "CircuitBreaker" → 서킷 상태 전이 추이
   - CLOSED->OPEN: 연속 실패로 차단
   - HALF_OPEN->OPEN: 프로브 실패, 아직 미복구
   - HALF_OPEN->CLOSED: 복구 완료 (info 레벨)
2. grep "connect failed" → Redis/PG 연결 실패
3. grep "produce failed" / "delivery failed" → Kafka 발행 실패
4. grep "TLS.*failed" → TLS 핸드셰이크 실패
```

### 인증/JWT 문제

```
1. grep "key not loaded" → RSA 키 파일 미로드 (Auth: private, Gateway: public)
2. grep "blacklist.*failed" → Redis 오류로 블랙리스트 체크 실패
3. grep "token reuse" → **보안 이벤트** — 토큰 탈취 가능성
4. grep "CSPRNG" → 시스템 엔트로피 고갈
```

---

## 6. 컴포넌트 맵

### apex_core (프레임워크) — 로거: `"apex"`

| 컴포넌트 | 코어 바인딩 | 패턴 | 소스 |
|----------|------------|------|------|
| Server | NO_CORE | 멤버 | server.hpp |
| CoreEngine | NO_CORE | 멤버 | core_engine.hpp |
| SessionManager | **per-core** | 멤버 | session_manager.hpp |
| TcpAcceptor | NO_CORE | 멤버 | tcp_acceptor.hpp |
| Listener | NO_CORE | 멤버 | listener.hpp |
| MessageDispatcher | NO_CORE | 멤버 | message_dispatcher.hpp |
| SpscMesh | NO_CORE | 멤버 | spsc_mesh.hpp |
| BumpAllocator | NO_CORE | 멤버 | bump_allocator.hpp |
| Config | NO_CORE | static fn | config.cpp |
| Session | NO_CORE | static fn | session.cpp |
| FrameCodec | NO_CORE | static fn | frame_codec.cpp |
| SlabAllocator | NO_CORE | static fn | slab_allocator.cpp |
| RingBuffer | NO_CORE | static fn | ring_buffer.cpp |
| WireHeader | NO_CORE | static fn | wire_header.cpp |
| ErrorSender | NO_CORE | static fn | error_sender.cpp |
| CrossCoreDispatcher | NO_CORE | static fn | cross_core_dispatcher.cpp |
| PeriodicTaskScheduler | NO_CORE | static fn | periodic_task_scheduler.cpp |

### apex_shared (어댑터) — 로거: `"app"`

| 컴포넌트 | 소속 | 소스 |
|----------|------|------|
| RedisAdapter | redis | redis_adapter.hpp |
| RedisConnection | redis | redis_connection.hpp |
| RedisMultiplexer | redis | redis_multiplexer.hpp |
| PgAdapter | pg | pg_adapter.hpp |
| PgConnection | pg | pg_connection.hpp |
| PgPool | pg | pg_pool.hpp |
| PgTransaction | pg | pg_transaction.hpp |
| KafkaAdapter | kafka | kafka_adapter.hpp |
| KafkaProducer | kafka | kafka_producer.hpp |
| KafkaConsumer | kafka | kafka_consumer.hpp |
| CircuitBreaker | common | circuit_breaker.hpp |
| TlsTcpTransport | protocols/tcp | tls_tcp_transport.hpp |

### 서비스 — 로거: `"app"`

| 컴포넌트 | 서비스 | 소스 |
|----------|--------|------|
| GatewayPipeline | gateway | gateway_pipeline.hpp |
| GatewayService | gateway | (ServiceBase 상속) |
| MessageRouter | gateway | message_router.hpp |
| ResponseDispatcher | gateway | response_dispatcher.hpp |
| PendingRequests | gateway | pending_requests.hpp |
| PubSubListener | gateway | pubsub_listener.hpp |
| BroadcastFanout | gateway | broadcast_fanout.hpp |
| JwtVerifier | gateway | jwt_verifier.hpp |
| JwtBlacklist | gateway | jwt_blacklist.hpp |
| FileWatcher | gateway | file_watcher.hpp |
| ConfigReloader | gateway | config_reloader.hpp |
| AuthService | auth-svc | (ServiceBase 상속) |
| JwtManager | auth-svc | jwt_manager.hpp |
| PasswordHasher | auth-svc | password_hasher.hpp |
| SessionStore | auth-svc | session_store.hpp |
| ChatService | chat-svc | (ServiceBase 상속) |
| ChatDbConsumer | chat-svc | chat_db_consumer.hpp |

**참고**: `(ServiceBase 상속)` 컴포넌트는 `ServiceBase::logger_`를 사용하며, `on_configure` 시점에 서비스명+core_id로 초기화된다. 모든 서비스 컴포넌트는 `NO_CORE`로 선언되나, ServiceBase 경유 시 `per-core`가 된다.
