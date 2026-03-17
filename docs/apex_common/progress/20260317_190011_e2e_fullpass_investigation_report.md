# E2E 11/11 전체 통과 + 개선 조사 보고서

> **작성일**: 2026-03-17
> **브랜치**: `feature/e2e-full-pass` @ `25be672`
> **현재 버전**: v0.5.5.1
> **범위**: E2E 근본 원인 수정 5건 + 프레임워크/아키텍처/CI 개선 조사

---

## 목차

1. [E2E 테스트 수정 요약](#1-e2e-테스트-수정-요약)
2. [프레임워크 레벨 개선 사항 (F1-F7)](#2-프레임워크-레벨-개선-사항-f1-f7)
3. [아키텍처 위반 감사 (W1-W4)](#3-아키텍처-위반-감사-w1-w4)
4. [TOML 설정 리밸런싱](#4-toml-설정-리밸런싱)
5. [Sanitizer 현황 및 UBSAN/Valgrind 검토](#5-sanitizer-현황-및-ubsanvalgrind-검토)
6. [백로그 대조 및 우선순위 권장](#6-백로그-대조-및-우선순위-권장)
7. [부록: 정상 구현 확인 사항](#7-부록-정상-구현-확인-사항)

---

## 1. E2E 테스트 수정 요약

### 1.1 최종 결과

| 테스트 스위트 | 테스트명 | 결과 | 소요 시간 |
|---|---|---|---|
| AuthE2ETest | LoginAndAuthenticatedRequest | PASS | 1.2s |
| AuthE2ETest | UnauthenticatedRequestRejected | PASS | 0s |
| AuthE2ETest | RefreshTokenRenewal | PASS | 32.4s |
| ChatE2ETest | RoomMessageBroadcast | PASS | 4.0s |
| ChatE2ETest | ListRooms | PASS | 1.5s |
| ChatE2ETest | GlobalBroadcast | PASS | 2.4s |
| RateLimitE2ETest | PerUserRateLimit | PASS | 1.8s |
| RateLimitE2ETest | PerIpRateLimit | PASS | 3.0s |
| RateLimitE2ETest | PerEndpointRateLimit | PASS | 1.5s |
| TimeoutE2ETest | ServiceTimeout | PASS | 7.3s |
| TimeoutE2ETest | ServiceRecoveryAfterTimeout | PASS | 2.2s |

**총 소요**: ~87초, **19개 파일 변경** (+431, -90)

### 1.2 근본 원인 5건 상세

#### Fix 1: RedisRateLimiter — hiredis 파라미터화 command

**파일**: `apex_shared/lib/rate_limit/src/redis_rate_limiter.cpp`

**근본 원인**: `RedisMultiplexer::command(string_view)` (deprecated)는 hiredis의 `redisAsyncCommand`에 format string으로 전달됨. `%` 지정자가 없으면 hiredis가 공백 기준으로 토큰을 분리하는데, 멀티라인 Lua 스크립트에 공백/줄바꿈이 다수 포함되어 파싱이 깨짐.

**수정 내용**: `load_script()`와 `execute_lua()` 모두 파라미터화된 `command(const char* fmt, Args&&...)` 사용으로 전환. `%s`가 hiredis RESP bulk string을 올바르게 생성하여 공백 분리 방지.

```cpp
// 수정 전 (BAD) — 공백에서 분리됨
multiplexer_.command("EVAL " + lua_script + " 2 key1 key2");

// 수정 후 (GOOD) — %s가 bulk string 생성
multiplexer_.command("EVAL %s 2 %s %s %s %s %s",
    script.c_str(), cur.c_str(), prev.c_str(),
    limit_str.c_str(), window_str.c_str(), now_str.c_str());
```

**영향 받은 테스트**: PerUserRateLimit, PerEndpointRateLimit

**hiredis 동작 원리 (보충)**:
- `redisAsyncCommand(ctx, cb, data, "EVAL %s 2 %s %s", ...)` → `%s`마다 RESP bulk string (`$len\r\ndata\r\n`) 생성
- `redisAsyncCommand(ctx, cb, data, "EVAL script 2 key1 key2")` → `%` 없으면 공백 토큰화 후 각각 bulk string
- Lua 스크립트 내 `local current = KEYS[1]` 같은 줄이 `local`, `current`, `=`, `KEYS[1]`로 분리되어 Redis가 파싱 실패

---

#### Fix 2: PubSubListener — post_init 후기 와이어링

**파일**: `apex_services/gateway/include/apex/gateway/gateway_service.hpp`, `apex_services/gateway/src/main.cpp`

**근본 원인**: Server의 서비스 팩토리(line 133-152)는 `server.run()` 호출 전에 실행되는데, `PubSubListener`는 `post_init_callback` 안에서 생성됨(line 200). 팩토리 시점에 `pubsub_listener.get()`은 null이므로 GatewayService의 `pubsub_listener_` 멤버가 영구적으로 null 상태.

**수정 내용**:
1. `GatewayService`에 `set_pubsub_listener(PubSubListener*)` 세터 추가
2. `post_init_callback`에서 PubSubListener 생성 → `start()` 호출 → 모든 코어의 GatewayService에 후기 와이어링

```cpp
// main.cpp post_init_callback 내부 (line 208-215)
for (uint32_t i = 0; i < num_cores; ++i) {
    auto* gw_svc = dynamic_cast<apex::gateway::GatewayService*>(
        srv.per_core_state(i).services[0].get());
    gw_svc->set_pubsub_listener(pubsub_listener.get());
}
```

**영향 받은 테스트**: RoomMessageBroadcast (room 채널 subscribe가 Redis에 도달하지 않던 문제)

**서비스 초기화 순서 (보충)**:
```
server.add_service_factory()  ← 팩토리 등록 (아직 실행 안 됨)
server.run()
  ├── CoreEngine 생성
  ├── 팩토리 실행 → GatewayService 인스턴스 생성 (pubsub_listener = null)
  ├── post_init_callback 실행
  │   ├── ResponseDispatcher 생성
  │   ├── BroadcastFanout 생성
  │   ├── PubSubListener 생성 + start()
  │   ├── GatewayService에 pubsub_listener 와이어링 ← Fix 2
  │   ├── Rate limiter 생성 + 와이어링
  │   └── SweepState 타이머 시작
  ├── ServiceBase::on_start() 호출
  └── io_context::run() 시작 → 메시지 수신 시작
```

---

#### Fix 3: GlobalBroadcast — Redis PubSub/Kafka 레이스 컨디션

**파일**: `apex_services/tests/e2e/e2e_chat_test.cpp`

**근본 원인**: alice가 GlobalBroadcast 요청 시, Gateway는 두 가지 경로로 메시지를 전달:
1. **Kafka 경로**: Gateway → Kafka → ChatService → Kafka response → Gateway → alice (response, msg_id=2042)
2. **PubSub 경로**: ChatService → Redis PUBLISH → PubSubListener → BroadcastFanout → alice (broadcast, msg_id=2043)

PubSub 경로가 Kafka 라운드트립보다 빠를 수 있어서, alice가 broadcast(2043)를 response(2042)보다 먼저 수신하는 경우 발생.

**수정 내용**: 두 메시지를 순서 무관하게 수집 후 존재 여부만 검증.

```cpp
auto msg1 = alice.recv(std::chrono::seconds{5});
auto msg2 = alice.recv(std::chrono::seconds{5});
bool got_response = (msg1.msg_id == 2042 || msg2.msg_id == 2042);
bool got_broadcast = (msg1.msg_id == 2043 || msg2.msg_id == 2043);
EXPECT_TRUE(got_response) << "Expected GlobalBroadcastResponse (2042)";
EXPECT_TRUE(got_broadcast) << "Expected GlobalChatMessage (2043)";
```

**영향 받은 테스트**: GlobalBroadcast

**메시지 경로 상세 (보충)**:
```
Alice → Gateway (msg_id=2042 GlobalBroadcastRequest)
  ├── Gateway → Kafka(chat.requests) → ChatService
  │   ├── ChatService → Kafka(gateway.responses) → Gateway → Alice (response 2042)
  │   └── ChatService → Redis PUBLISH(pub:global:chat) → PubSubListener
  │       → BroadcastFanout → cross_core_post → ALL sessions (broadcast 2043)
  └── 두 경로 중 PubSub이 더 빠를 수 있음 (Redis in-memory vs Kafka disk)
```

---

#### Fix 4: Rate Limit 윈도우 단축 + 테스트 간 격리

**파일**: `apex_services/tests/e2e/gateway_e2e.toml`, `apex_services/tests/e2e/e2e_ratelimit_test.cpp`

**근본 원인 (이중)**:
1. **User/Endpoint 윈도우 60초**: PerUserRateLimit에서 alice의 예산을 소진하면, 60초간 같은 유저(alice)를 사용하는 ServiceRecoveryAfterTimeout이 rate limit에 걸림
2. **IP 윈도우 10초**: PerIpRateLimit이 50개 요청으로 IP를 소진하면, 10초간 같은 IP의 모든 새 연결이 거부됨 → 후속 테스트 실패

**수정 내용**:
```toml
# gateway_e2e.toml — 모든 윈도우 E2E용 단축
[rate_limit.ip]
total_limit = 30
window_size_seconds = 2   # 원래 10s

[rate_limit.user]
default_limit = 10
window_size_seconds = 3   # 원래 60s

[rate_limit.endpoint]
default_limit = 60
window_size_seconds = 3   # 원래 60s
```

```cpp
// e2e_ratelimit_test.cpp PerIpRateLimit 테스트 끝
// 윈도우 만료 대기 (window=2s + 마진 1s = 3s)
std::this_thread::sleep_for(std::chrono::seconds{3});
```

**영향 받은 테스트**: PerEndpointRateLimit, ServiceRecoveryAfterTimeout

**Rate Limit 파이프라인 순서 (보충)**:
```
클라이언트 요청 → GatewayPipeline
  1. Per-IP Rate Limit (PerIpRateLimiter, 로컬 메모리) → 107 RateLimitedIp
  2. JWT 검증 (JwtVerifier)
  3. Per-User Rate Limit (RedisRateLimiter, Redis Lua) → 108 RateLimitedUser
  4. Per-Endpoint Rate Limit (RedisRateLimiter, Redis Lua) → 109 RateLimitedEndpoint
  5. MessageRouter → Kafka produce
```

---

#### Fix 5: PubSub Subscribe 타이밍

**파일**: `apex_services/tests/e2e/e2e_chat_test.cpp`

**근본 원인**: PubSubListener는 별도 스레드에서 `select()` 기반 1초 주기 폴링. `subscribe()` 호출 시 `has_pending_subs_` atomic flag만 설정하고, 실제 Redis SUBSCRIBE 명령은 다음 `select()` 타임아웃 시 `apply_pending_subscriptions()`에서 전송. 기존 200ms sleep으로는 구독이 적용되기 전에 메시지를 보내는 경우 발생.

**수정 내용**: subscribe 후 sleep을 200ms → 1500ms로 증가.

```cpp
// PubSubListener의 select() 1초 주기 + Redis SUBSCRIBE 왕복 시간 고려
std::this_thread::sleep_for(std::chrono::milliseconds{1500});
```

**영향 받은 테스트**: RoomMessageBroadcast

**PubSubListener 내부 루프 (보충)**:
```
run_thread():
  while (!stop_requested_) {
    if (has_pending_subs_.load(acquire)) {
      apply_pending_subscriptions();  // Redis SUBSCRIBE 명령 전송
      has_pending_subs_.store(false, release);
    }
    select(fd, timeout=1s);  // Redis 응답 대기
    if (readable) {
      redisGetReply();  // 메시지 수신 → on_message_ 콜백
    }
  }
```

---

## 2. 프레임워크 레벨 개선 사항 (F1-F7)

현재 `gateway/src/main.cpp`의 `post_init_callback`이 **250줄 넘는 초기화 코드**를 담고 있음. 서비스가 생성된 후에야 준비되는 의존성을 수동으로 와이어링하는 패턴이 반복됨.

### F1: 초기화 순서 의존 — `on_init_phase()` 훅 필요

**현재 해킹**: `set_pubsub_listener()`, `set_rate_limiter()` 등 후기 세터 메서드. 서비스 factory에서 null로 생성 → post_init에서 수동 와이어링.

**문제점**:
- 서비스당 의존성 1개마다 세터 1개 + null 체크 1개 필요
- 서비스가 `on_start()` 전에 null 상태로 존재하는 시간 구간 발생
- post_init_callback이 모든 서비스의 와이어링을 중앙 집중 처리 → main.cpp 비대화

**제안**: ServiceBase에 `on_init_phase(InitContext&)` 가상 메서드 추가

```cpp
class ServiceBase {
    // 현재: on_start(), on_stop()만 존재
    // 제안: factory 이후, on_start() 이전에 호출
    virtual void on_init_phase(const InitContext& ctx) {}
};

struct InitContext {
    Server& server;
    std::vector<ServiceBaseInterface*> all_services;
    std::vector<SessionManager*> session_mgrs_per_core;
};
```

**기대 효과**: 각 서비스가 자신의 초기화를 스스로 처리. post_init_callback에서 서비스별 와이어링 코드 제거.

**예상 규모**: 2-3일

---

### F2: Raw 포인터 DI — 타입 안전 의존성 주입

**현재 해킹**:
```cpp
// gateway_service.hpp
struct Dependencies {
    ChannelSessionMap* channel_map = nullptr;       // 누가 소유?
    PubSubListener* pubsub_listener = nullptr;      // nullable, 언제 유효?
    RateLimitFacade* rate_limiter = nullptr;         // nullable, 언제 유효?
};
```

**문제점**:
- 소유권이 코드에서 명시되지 않음 (main.cpp의 스코프 변수가 소유)
- nullable raw 포인터 → 런타임 null 체크 필수 (핫 패스에서 분기)
- rate_limiter는 `atomic<T*>`이지만 다른 포인터는 비atomic (불일치)

**제안**: `optional<reference_wrapper<T>>`로 nullable 의존성 표현

```cpp
struct Dependencies {
    ChannelSessionMap& channel_map;                                    // 필수 (참조)
    std::optional<std::reference_wrapper<PubSubListener>> pubsub;      // 선택
    std::optional<std::reference_wrapper<RateLimitFacade>> rate_limiter;// 선택
};
```

**예상 규모**: 1-2일

---

### F3: Standalone 어댑터 — 역할별 다중 어댑터 등록

**현재 해킹**: Rate limit Redis 어댑터가 Server의 어댑터 레지스트리 밖에서 수동 생성.

```cpp
// main.cpp:93-99 — Server 레지스트리와 별도로 생성
auto rl_redis_adapter = std::make_unique<RedisAdapter>(redis_rl_cfg);

// main.cpp:277 — post_init에서 수동 init
rl_redis_adapter->do_init(srv.core_engine());
```

**문제점**:
- `server.adapter<RedisAdapter>()`가 하나의 타입당 하나의 인스턴스만 반환
- Gateway는 Redis 3개(Auth, PubSub, RateLimit)가 필요하지만 레지스트리에 등록 불가
- 수동 init 호출이 Server의 라이프사이클과 분리되어 관리 포인트 증가

**제안**: 역할(role) 파라미터 지원

```cpp
server
    .add_adapter<RedisAdapter>("auth", redis_auth_cfg)
    .add_adapter<RedisAdapter>("ratelimit", redis_rl_cfg);

// 사용 시
auto& rl_redis = server.adapter<RedisAdapter>("ratelimit");
```

**예상 규모**: 1일

---

### F4: 수동 주기적 태스크 — SweepState 보일러플레이트

**현재 해킹**: main.cpp:229-273에서 `enable_shared_from_this` 기반 45줄 구조체.

```cpp
struct SweepState : std::enable_shared_from_this<SweepState> {
    std::vector<PendingRequestsMap*> pending;
    std::vector<SessionManager*> sessions;
    CoreEngine* engine = nullptr;
    std::shared_ptr<boost::asio::steady_timer> timer;

    void schedule() {
        timer->expires_after(std::chrono::seconds{1});
        timer->async_wait([self = shared_from_this()](auto ec) {
            if (ec) return;
            self->sweep();
            self->schedule();
        });
    }
    void sweep() { /* 각 코어에 post하여 expired 항목 정리 */ }
};
```

**문제점**:
- 모든 주기적 태스크마다 동일 보일러플레이트 반복 필요
- 타이머 취소/셧다운 시 lifetime 관리가 수동
- 향후 메트릭 flush, 토큰 정리, 캐시 TTL 등에도 동일 패턴 필요

**제안**: 프레임워크 레벨 `PeriodicTaskScheduler`

```cpp
server.schedule_periodic_task({
    .interval = std::chrono::seconds{1},
    .task = [](uint32_t core_id, PerCoreState& state) { /* sweep */ },
    .cross_core_broadcast = true,
    .name = "timeout-sweep"
});
```

**기대 효과**: 45줄 → ~10줄, 셧다운 시 자동 타이머 취소, 인스트루먼테이션 가능

**예상 규모**: 2-3일

---

### F5: Per-core 와이어링 루프 — 컴포넌트 레지스트리

**현재 해킹**: 동일한 `for(core) { dynamic_cast + 포인터 추출 }` 루프가 main.cpp에 **4회** 반복:
- Line 165-171: PendingRequestsMap* 수집 (ResponseDispatcher용)
- Line 191-194: SessionManager* 수집 (BroadcastFanout용)
- Line 219-227: PendingRequestsMap* + SessionManager* 재수집 (SweepState용 — 이전 벡터가 move됨)
- Line 289-330: PerCoreRateLimit 생성 + GatewayService 와이어링

**문제점**:
- `dynamic_cast<GatewayService*>(state.services[0].get())` — null 체크 없음 3회
- 같은 포인터를 반복 수집 (move 후 재수집하는 안티패턴)
- 서비스 타입과 인덱스에 하드코딩 의존

**제안**: Per-core 컴포넌트 레지스트리

```cpp
server.add_per_core_component("rate_limiters",
    [&](uint32_t core, PerCoreState& state) {
        // 한 번만 작성, 프레임워크가 코어별 반복 처리
    });

auto rate_limiters = server.per_core_components<RateLimitFacade>("rate_limiters");
```

**예상 규모**: 2일

---

### F6: 라이프사이클 훅 부족

**현재**: ServiceBase에 `on_start()`와 `on_stop()`만 존재.

**문제점**: factory → on_start() 사이에 의존성 주입할 훅이 없음. post_init_callback이 이 갭을 메우지만, 모든 서비스의 초기화가 main.cpp 한 곳에 집중됨.

**제안**: `on_initialize()` 훅 추가 (F1과 동일한 해결책)

**관계**: F1과 F6은 사실상 같은 작업. `on_init_phase(InitContext&)` 하나로 해결.

**예상 규모**: 1일 (F1에 포함)

---

### F7: CoreEngine vs Server 이원화

**현재 상황**:
- Gateway: `Server` 사용 (multi-core, adapter registry, service factory)
- Auth Service: `CoreEngine` 직접 사용 (standalone)
- Chat Service: `CoreEngine` 직접 사용 (standalone)

**문제점**:
- Auth/Chat은 `adapter.init(engine)` 수동 호출, Server 경유 시 자동
- 서비스 코드가 Server와 CoreEngine 두 가지 초기화 경로를 알아야 함
- standalone 패턴이 문서화되지 않음 (백로그 C-1)

**제안**: Server가 단일 서비스도 지원하거나, CoreEngine 래퍼 제공

**예상 규모**: 1-2일

---

### F1-F7 요약 테이블

| # | 문제 | 현재 해킹 | 프레임워크 해결책 | 규모 | 우선순위 |
|---|------|----------|-----------------|------|---------|
| F1 | 초기화 순서 의존 | `set_*()` 후기 세터 | `on_init_phase(InitContext&)` | 2-3일 | **High** |
| F2 | Raw 포인터 DI | nullable `T*` + null 체크 | `optional<reference_wrapper<T>>` | 1-2일 | Medium |
| F3 | Standalone 어댑터 | 수동 생성/init | `add_adapter<T>(role)` | 1일 | Low |
| F4 | 주기적 태스크 | SweepState 45줄 | `schedule_periodic_task()` | 2-3일 | Medium |
| F5 | Per-core 루프 반복 | dynamic_cast 4회 | Per-core 컴포넌트 레지스트리 | 2일 | Low |
| F6 | 라이프사이클 훅 부족 | post_init_callback | `on_initialize()` (F1과 동일) | F1 포함 | **High** |
| F7 | CoreEngine/Server 이원화 | 두 가지 init 경로 | 통합 서비스 API | 1-2일 | Medium |

**핵심**: F1+F6 해결 시 post_init_callback 250줄 → 각 서비스의 `on_init_phase()`로 분산, main.cpp ~40% 간결화.

---

## 3. 아키텍처 위반 감사 (W1-W4)

### 감사 결과 요약

- **CRITICAL 위반**: **0건** (데이터 레이스, UB 위험 없음)
- **WARNING**: **4건** (현재 동작하지만 위험한 패턴)
- **shared-nothing 원칙**: **올바르게 유지됨**

---

### W1: dynamic_cast 무검증 (WARNING)

**위치**: `main.cpp:167-168, 212-213, 327-328`

```cpp
auto* gw_svc = dynamic_cast<apex::gateway::GatewayService*>(
    state.services[0].get());
// null 체크 없음 → services[0]이 GatewayService가 아니면 nullptr deref
```

**현재 안전한 이유**: Gateway 프로세스에 GatewayService가 유일한 서비스이므로 `services[0]`이 항상 GatewayService.

**위험 시나리오**: 멀티 서비스 배치, 서비스 순서 변경, 빈 services 벡터.

**권장**: `server.service<GatewayService>(core_id)` 타입 안전 접근 (F5 해결 시 자동 해소)

---

### W2: Raw 포인터 벡터 (WARNING)

**위치**: `main.cpp:162-170, 191-196, 219-227`

```cpp
std::vector<PendingRequestsMap*> pending_maps;
std::vector<SessionManager*> session_mgrs;
```

**현재 안전한 이유**: Server가 모든 객체를 소유하고, `server.run()` 종료 전까지 lifetime 보장.

**위험 시나리오**: 서비스 동적 리로드, 핫스왑, 서비스 재시작.

**추가 문제**: Line 219-227에서 이전에 move된 벡터를 **재수집**하는 패턴 — move 후 원본은 유효하지 않으므로 새로 수집하는 것은 맞지만, 같은 데이터를 두 번 수집하는 것 자체가 설계 냄새.

---

### W3: 2단계 초기화 (WARNING)

**위치**: `gateway_service.hpp:44`, `main.cpp:147,214`

**흐름**:
1. Factory: `deps.pubsub_listener = nullptr` (PubSubListener가 아직 없음)
2. GatewayService 생성: `pubsub_listener_ = nullptr`
3. post_init: PubSubListener 생성 → `set_pubsub_listener()` 호출
4. on_start(): `pubsub_listener_`이 유효해야 함

**현재 안전한 이유**: Server가 `on_start()` 전에 post_init_callback을 완료함. 메시지 디스패치는 `on_start()` 이후 시작.

**위험**: assertion이 없어서, 만약 라이프사이클 순서가 바뀌면 silent null deref.

**권장**: `on_start()`에서 `assert(pubsub_listener_ != nullptr)` 추가, 또는 F1로 해소.

---

### W4: Raw 포인터 DI (WARNING)

**위치**: `gateway_service.hpp:37-46`

**Dependencies 구조체 소유권 분석**:

| 멤버 | 타입 | 소유자 | 안전성 |
|------|------|--------|--------|
| `kafka` | `KafkaAdapter&` | Server adapter registry | OK (참조) |
| `jwt_verifier` | `const JwtVerifier&` | main.cpp 스코프 | OK (const 참조) |
| `jwt_blacklist` | `JwtBlacklist*` | 미사용 (nullptr) | OK |
| `route_table` | `shared_ptr` | 공유 소유 | OK |
| `channel_map` | `ChannelSessionMap*` | main.cpp 스코프 | **암묵적** |
| `pubsub_listener` | `PubSubListener*` | main.cpp 스코프 | **암묵적 + nullable** |
| `rate_limiter` | `RateLimitFacade*` | main.cpp per_core_rl 벡터 | **암묵적 + nullable** |

**문제**: raw 포인터 3개의 lifetime이 main.cpp 스코프 변수에 의존하지만, 이를 타입 시스템이 강제하지 않음.

---

### W1-W4와 F1-F7의 관계

| 경고 | 해소하는 프레임워크 개선 |
|------|------------------------|
| W1 (dynamic_cast) | F5 (서비스 레지스트리) |
| W2 (raw ptr 벡터) | F5 (per-core 컴포넌트 레지스트리) |
| W3 (2단계 초기화) | F1+F6 (`on_init_phase()`) |
| W4 (raw ptr DI) | F2 (`optional<reference_wrapper<T>>`) |

---

### 정상 구현 확인 (EXCELLENT 판정)

| 컴포넌트 | 동기화 메커니즘 | 판정 |
|----------|---------------|------|
| `ChannelSessionMap` | `shared_mutex` (reader-biased) | EXCELLENT — broadcast >> subscribe 패턴에 최적 |
| `BroadcastFanout` | `cross_core_post()` + `shared_ptr` 데이터 공유 | EXCELLENT — 각 코어 자체 스레드에서 SessionManager 접근 |
| `ResponseDispatcher` | `asio::post(target_core)` + 방어적 검증 | EXCELLENT — 페이로드 `shared_ptr` 공유, 코어 범위 체크 |
| `PubSubListener` | `atomic<bool>` acquire/release + `mutex` | EXCELLENT — 깔끔한 스레드 경계 |
| `GatewayPipeline::rate_limiter_` | `atomic<T*>` acquire/release | EXCELLENT — 유일한 cross-thread 포인터 |
| `SweepState` | `enable_shared_from_this` + 타이머 | EXCELLENT — 올바른 async 패턴 |
| `PerCoreRateLimit` | main 스코프 RAII | EXCELLENT — server.run() 이후 파괴 |

---

## 4. TOML 설정 리밸런싱

### 4.1 현재 값과 권장 값

| 설정 | 파일 | 현재 (E2E) | CI/TSAN 권장 | 프로덕션 | 비고 |
|------|------|-----------|-------------|---------|------|
| IP rate limit window | gateway_e2e.toml | **2s** | **4-5s** | 60s | TSAN 5-10x 슬로우다운 대응 |
| IP rate limit total | gateway_e2e.toml | 30 | 30 | 1000 | |
| User rate limit window | gateway_e2e.toml | **3s** | **3-4s** | 300s | |
| User rate limit default | gateway_e2e.toml | 10 | 10 | 100 | |
| Endpoint rate limit window | gateway_e2e.toml | **3s** | **3-4s** | 300s | |
| Endpoint rate limit default | gateway_e2e.toml | 60 | 60 | 600 | |
| Endpoint override (2001) | gateway_e2e.toml | 5 | 5 | 30 | CreateRoom |
| Access token TTL | auth_svc_e2e.toml | 30s | 30s (OK) | 3600s | Kafka 라운드트립 충분 |
| Request timeout | gateway_e2e.toml | 5000ms | 5000ms (OK) | 30000ms | |
| PerIpRateLimit 후 sleep | e2e_ratelimit_test.cpp | 3s | **5-6s** | N/A | 윈도우 만료 대기 |
| PubSub subscribe sleep | e2e_chat_test.cpp | 1500ms | 1500ms (OK) | N/A | select() 1초 + 마진 |

### 4.2 TSAN 슬로우다운 분석

**TSAN 오버헤드**: 일반적으로 5-10x CPU 시간 증가.

**IP 윈도우 (2s) 위험성**:
- PerIpRateLimit 테스트가 50개 요청을 burst로 전송
- TSAN 하에서 루프 1회당 ~100ms (일반: ~10ms)
- 50회 × 100ms = 5초 → 2초 윈도우가 2.5번 리셋됨
- 각 윈도우에 ~10개 요청만 들어가므로 total_limit=30에 도달 못할 위험

**해결 전략**: CI 전용 TOML을 별도로 만들거나, 기존 E2E TOML의 윈도우를 4-5초로 올리되 sleep도 비례 증가.

### 4.3 설정 분리 전략

```
apex_services/tests/e2e/
  gateway_e2e.toml          # 로컬 개발용 (현재)
  gateway_e2e_ci.toml       # CI용 (TSAN 대응 윈도우)
  gateway.toml              # 프로덕션 (참고용 — 실제 배포는 K8s ConfigMap)
```

또는 환경 변수 오버라이드를 TOML 파서에 지원하여 단일 파일로 관리.

---

## 5. Sanitizer 현황 및 UBSAN/Valgrind 검토

### 5.1 현재 Sanitizer 지원 상태

| Sanitizer | CMake 프리셋 | MSVC | Clang/GCC | CI Job | Suppressions |
|-----------|-------------|------|-----------|--------|-------------|
| **ASAN** | `asan` / `asan-msvc` | `/fsanitize=address` | `-fsanitize=address` | `linux-asan` | `lsan_suppressions.txt` (spdlog registry) |
| **TSAN** | `tsan` | X (미지원) | `-fsanitize=thread` | `linux-tsan` | `tsan_suppressions.txt` (Boost.Asio, mutex) |
| **UBSAN** | **없음** | **없음** | **없음** | **없음** | **없음** |
| **Valgrind** | N/A | N/A | Linux only | N/A | N/A |

### 5.2 UBSAN 추가 작업

**필요 작업**:

1. **CMakePresets.json** — 프리셋 2개 추가:
   - `ubsan` (Clang/GCC): `-fsanitize=undefined -fno-omit-frame-pointer`
   - `ubsan-msvc` (MSVC): `/fsanitize=undefined` (MSVC 17.4+)

2. **테스트 프리셋**:
   - `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`

3. **CI workflow** (`.github/workflows/ci.yml`):
   - `linux-ubsan` job 추가 (matrix)

4. **Suppressions 파일** (`ubsan_suppressions.txt`):
   - signed overflow (timing wheel 계산)
   - shift operations on signed integers
   - 예상: 초기에는 빈 파일, 빌드 후 필요 시 추가

**예상 작업량**: 반나절

**ASAN+UBSAN 통합 가능성**: Clang/GCC에서는 `-fsanitize=address,undefined`로 결합 가능. 하나의 job으로 통합하면 CI 시간 절약. 백로그 I-15에서도 "ASAN+UBSAN: 하나의 job" 명시.

### 5.3 Valgrind 검토 결과

**결론: 드롭 권장**

| 관점 | Valgrind | ASAN |
|------|----------|------|
| 플랫폼 | Linux only | Windows + Linux |
| 오버헤드 | 20-50x | 2x |
| 메모리 감지 | use-after-free, leak, uninit | use-after-free, overflow, leak |
| CI 시간 | 매우 느림 | 빠름 |
| 정밀도 | 높음 (바이트 단위) | 높음 (shadow memory) |

- ASAN이 Valgrind의 메모리 이슈 대부분을 커버
- ASAN 실행 속도가 10-25x 빠름
- Windows에서 Valgrind 사용 불가
- 백로그 I-15에서 Valgrind을 별도 job으로 명시했으나, 실용성 대비 CI 비용이 과도

**권장**: 백로그 I-15에서 Valgrind 항목 제거, ASAN+UBSAN 통합 job으로 대체.

### 5.4 현재 CI 파이프라인 구조

```yaml
# .github/workflows/ci.yml — 현재 5개 job
jobs:
  linux-gcc:      # Ubuntu, debug 프리셋, Docker
  linux-asan:     # Ubuntu, asan 프리셋, Docker
  linux-tsan:     # Ubuntu, tsan 프리셋, Docker (kafka 테스트 필터)
  windows-msvc:   # Windows, debug 프리셋
  root-linux-gcc: # Ubuntu, 루트 빌드 무결성 검증
```

**E2E 테스트 CI 부재**: 현재 E2E는 `ctest -LE e2e`로 제외됨. Docker Compose 인프라가 CI에서 기동되어야 하므로 별도 job 필요.

### 5.5 CI E2E 파이프라인 구축 방안

```yaml
# 제안: 신규 job
linux-e2e:
  runs-on: ubuntu-latest
  services:
    redis-auth:      # docker compose의 Redis 4종
    redis-pubsub:
    redis-ratelimit:
    redis-chat:
    kafka:           # KRaft 모드
    postgres:
  steps:
    - build (debug)
    - start Gateway + Auth + Chat services
    - run apex_e2e_tests
    - collect logs on failure
```

**예상 작업량**: 1-2일

---

## 6. 백로그 대조 및 우선순위 권장

### 6.1 관련 백로그 항목

| 백로그 ID | 중요도 | 내용 | 이번 사이클 관련성 |
|-----------|--------|------|-------------------|
| **C-1** | Critical | apex_core 프레임워크 아키텍처 문서 | F1-F7 작업의 설계 근거, v0.6 착수 전 필수 |
| **I-15** | Important | Linux CI Sanitizer 파이프라인 (ASAN+UBSAN+TSAN+Valgrind) | UBSAN 추가 + Valgrind 드롭 → **이번 사이클** |
| **I-17** | Important | E2E 테스트 실행 가이드 문서 | `apex_services/tests/e2e/CLAUDE.md`로 **해소** |
| **I-18** | Important | Server multi-listener dispatcher sync_all_handlers | 이번 사이클 밖 |
| **m-2** | Minor | 별도 백로그 파일 미이전 2건 | 정리 태스크 |
| **m-5** | Minor | docs-only 커밋에도 전체 빌드 실행 | `[skip ci]` workaround 존재 |

### 6.2 이번 사이클 (v0.5.5.x → v0.5.6) 권장 순서

| 순서 | 작업 | 규모 | 근거 |
|------|------|------|------|
| 1 | TOML 리밸런싱 — CI TSAN 대응 | 반나절 | CI E2E 파이프라인 전제 조건 |
| 2 | UBSAN 프리셋 추가 (ASAN+UBSAN 통합) | 반나절 | I-15 해소, 낮은 리스크 |
| 3 | Valgrind 드롭 — 백로그 I-15 수정 | 즉시 | ASAN으로 대체, CI 비용 절감 |
| 4 | CI E2E 파이프라인 구축 | 1-2일 | E2E 11/11 통과 후 자연스러운 다음 단계 |

### 6.3 v0.6 (운영 인프라) 권장 순서

| 순서 | 작업 | 규모 | 근거 |
|------|------|------|------|
| 5 | F1+F6: ServiceBase `on_init_phase()` 훅 | 2-3일 | post_init 해소, W3 해소 |
| 6 | F4: 주기적 태스크 스케줄러 | 2-3일 | SweepState + 향후 메트릭/정리 태스크 |
| 7 | F3: 역할별 다중 어댑터 | 1일 | standalone 어댑터 해소 |
| 8 | C-1: 아키텍처 문서 | 2-3일 | 위 변경사항 반영하여 작성 |

F2(타입 안전 DI), F5(per-core 레지스트리), F7(CoreEngine/Server 통합)은 F1+F6 적용 후 자연스럽게 정리되므로 별도 태스크 불필요.

---

## 7. 부록: 정상 구현 확인 사항

### 7.1 Shared-Nothing 아키텍처 검증

```
CoreEngine
  ├── Core 0: io_context_0 → SessionManager_0 → GatewayService_0
  ├── Core 1: io_context_1 → SessionManager_1 → GatewayService_1
  └── ...

공유 상태 (의도적):
  ChannelSessionMap — shared_mutex 보호

크로스코어 통신:
  cross_core_post(engine, target_core, lambda)
  → target_core의 io_context에 post
  → lambda는 target_core 스레드에서 실행
  → 모든 mutable state 접근은 해당 코어 스레드에서만
```

### 7.2 Rate Limit 파이프라인

```
클라이언트 → GatewayPipeline
  1. Per-IP (PerIpRateLimiter, 로컬 메모리, per-core)
     → ErrorCode::RateLimitedIp (107)
  2. JWT 검증 (JwtVerifier, stateless)
     → ErrorCode::AuthFailed (101)
  3. Per-User (RedisRateLimiter, Redis Lua script, per-core multiplexer)
     → ErrorCode::RateLimitedUser (108)
  4. Per-Endpoint (RedisRateLimiter, Redis Lua script, EndpointRateConfig override)
     → ErrorCode::RateLimitedEndpoint (109)
  5. MessageRouter → Kafka produce
```

### 7.3 PubSub 메시지 경로

```
Redis PUBLISH "pub:global:chat" <message>
  → PubSubListener (전용 스레드, select() 1초 폴링)
    → on_message_ 콜백 (BroadcastFanout::fanout)
      → global channel (pub:global:*):
          for each core: cross_core_post → SessionManager.for_each → session.enqueue_write
      → room channel:
          ChannelSessionMap.get_subscribers(channel) [shared_lock]
          group by core_id
          for each core: cross_core_post → session.enqueue_write
```

### 7.4 서비스 프로세스 구조 (E2E)

```
E2E Test Fixture (GTest)
  ├── docker compose up -d
  │   ├── apex-e2e-redis-auth     (port 6380)
  │   ├── apex-e2e-redis-pubsub   (port 6383)
  │   ├── apex-e2e-redis-ratelimit(port 6381)
  │   ├── apex-e2e-redis-chat     (port 6382)
  │   ├── apex-e2e-kafka          (port 9092)
  │   └── apex-e2e-postgres       (port 5433)
  ├── CreateProcessW: Gateway      (ws:8444, tcp:8443)
  ├── CreateProcessW: AuthService
  ├── CreateProcessW: ChatService
  └── TcpClient (테스트 코드) → Gateway:8443
      ├── WireHeader v2 (12B) + FlatBuffers payload
      └── recv() with timeout
```

### 7.5 수정된 파일 전체 목록

| 파일 | 변경 | 내용 |
|------|------|------|
| `CLAUDE.md` | M | E2E 가이드 포인터 추가 |
| `apex_services/chat-svc/src/chat_service.cpp` | M | (이전 수정) |
| `apex_services/gateway/include/apex/gateway/gateway_service.hpp` | M | `set_pubsub_listener()` 추가 |
| `apex_services/gateway/include/apex/gateway/pubsub_listener.hpp` | M | (이전 수정) |
| `apex_services/gateway/src/main.cpp` | M | PubSubListener 후기 와이어링 루프 추가 |
| `apex_services/gateway/src/pubsub_listener.cpp` | M | (이전 수정 — 구독 로직 개선) |
| `apex_services/tests/e2e/auth_svc_e2e.toml` | M | (이전 수정) |
| `apex_services/tests/e2e/e2e_auth_test.cpp` | M | (이전 수정) |
| `apex_services/tests/e2e/e2e_chat_test.cpp` | M | GlobalBroadcast 순서 무관 검증, subscribe sleep 1500ms |
| `apex_services/tests/e2e/e2e_ratelimit_test.cpp` | M | PerIpRateLimit 후 3초 sleep 추가 |
| `apex_services/tests/e2e/e2e_test_fixture.cpp` | M | (이전 수정) |
| `apex_services/tests/e2e/e2e_timeout_test.cpp` | M | (이전 수정) |
| `apex_services/tests/e2e/gateway_e2e.toml` | M | Rate limit 윈도우 단축 (IP=2s, User=3s, Endpoint=3s) |
| `apex_shared/lib/adapters/redis/include/.../redis_multiplexer.hpp` | M | (이전 수정) |
| `apex_shared/lib/adapters/redis/src/redis_adapter.cpp` | M | (이전 수정) |
| `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp` | M | (이전 수정) |
| `apex_shared/lib/rate_limit/src/redis_rate_limiter.cpp` | M | 파라미터화 command() 전환 |
| `apex_shared/tests/unit/test_redis_adapter.cpp` | M | (이전 수정) |
| `apex_services/tests/e2e/CLAUDE.md` | **A** | E2E 트러블슈팅 가이드 (신규) |
