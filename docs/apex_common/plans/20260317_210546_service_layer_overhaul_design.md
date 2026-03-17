# 서비스 레이어 전면 개선 — 설계 문서

**버전**: v0.6 선행 작업
**브랜치**: `feature/service-layer-overhaul`
**범위**: 코어 프레임워크 인터페이스 확장 + 서비스 3개 마이그레이션

---

## 1. 목적

코어 프레임워크(apex_core)의 인터페이스를 확장하여 서비스 레이어의 성능·안전성·일관성을 높인다.
전 서비스를 Server + ServiceBase 패턴으로 통일하고, 프레임워크가 초기화 순서·메모리 관리·메시지 라우팅 규약을 강제한다.

### 동기

- Auth/Chat이 CoreEngine을 직접 사용하며 ServiceBase를 상속하지 않음 → 라이프사이클 보장 없음
- Gateway의 `post_init_callback`이 250줄 거대 람다 → 초기화 순서 실수 위험
- 서비스마다 FlatBuffers 검증, Kafka envelope 직렬화, 주기적 작업 boilerplate 반복
- BumpAllocator/ArenaAllocator가 서비스 레이어에서 전혀 활용되지 않음

---

## 2. 설계 결정 요약

| ID | 결정 | 근거 |
|----|------|------|
| **D1** | 전 서비스 Server + ServiceBase 통일 | 프레임워크 규약의 일관성. Auth/Chat도 스케일아웃 대상 |
| **D2** | 다단계 라이프사이클 강제 (불필요 페이즈 스킵) | 초기화 순서 실수 원천 차단 |
| **D3** | CRTP 유지: 라이프사이클=virtual, 메시지 핸들링=template 기반 등록 | 라이프사이클 훅은 서비스당 1회 호출(cold path). 메시지 핸들링은 CRTP 템플릿으로 컴파일 타임 코드 생성, 런타임 디스패치는 hash map O(1) + std::function |
| **D4** | 어댑터 역할 문자열 다중 등록, flat 컨테이너 | 기존 API 하위 호환, 소규모 레지스트리 |
| **D5** | per-core 서비스 레지스트리 (타입 기반) | dynamic_cast 제거 |
| **D6** | `route<T>()` FlatBuffers 검증 내장 | 프레임워크=구조 검증, 서비스=비즈니스 검증 |
| **D7** | `kafka_route<T>()` 신규 — EnvelopeMetadata 값 전달 | Kafka 소비 서비스의 타입 안전한 메시지 소스 구분 |
| **D8** | `EnvelopeBuilder.build_into(BumpAllocator&)` 단일 API | 기본 경로=고성능, 힙 필요 시 서비스가 명시적 복사 |
| **D9** | ServiceBase가 `bump()`/`arena()` 접근자 자동 제공 | per-core 상태 접근 boilerplate 제거 |
| **D10** | 세션 종료 훅 `on_session_closed()` | auth_states_ 메모리 누수 해결 |
| **D11** | 주기적 작업 스케줄러 (WireContext에서 등록) | SweepState boilerplate 제거 |
| **D12** | Server는 리스너/어댑터 0개에도 정상 동작 | 엣지 케이스 크래시 방지 |

---

## 3. ServiceBase 다단계 라이프사이클

### 3.1 라이프사이클 흐름

```
Server.run()
  │
  ├─ 어댑터 init(CoreEngine&)
  ├─ per-core 서비스 팩토리 실행 + ServiceRegistry 등록
  │
  ├─ Phase 1: on_configure(ConfigureContext&)
  │    ├─ server.adapter<T>(role) 접근 가능
  │    ├─ bump()/arena() 자동 세팅 (internal_configure)
  │    └─ 다른 서비스 접근 불가 (Context에 레지스트리 없음)
  │
  ├─ Phase 2: on_wire(WireContext&)
  │    ├─ local_registry / global_registry 접근 가능
  │    ├─ scheduler.schedule() 등록 가능
  │    └─ 서비스 간 와이어링
  │
  ├─ Phase 3: on_start()
  │    ├─ route<T>() — TCP 핸들러 등록
  │    ├─ kafka_route<T>() — Kafka 핸들러 등록
  │    └─ set_default_handler() 등
  │
  ├─ Kafka 브릿지 자동 연결 (kafka_route 등록 시)
  ├─ CoreEngine.start()
  ├─ Listener.start() (리스너 있을 때)
  │
  ▼
  [메시지 처리 루프]
  │  on_session_closed() — 세션 종료 시 자동 호출
  │
  ▼
  Server.stop()
  │  on_stop() → 어댑터 drain/close → CoreEngine 종료
```

### 3.2 Context 객체

Context 타입으로 페이즈별 접근 범위를 **컴파일 타임 강제**한다. ConfigureContext에는 레지스트리 멤버가 없으므로
Phase 1에서 다른 서비스에 접근하는 코드는 컴파일 자체가 되지 않는다.

```cpp
// Phase 1: 어댑터만 접근 가능 — 서비스 레지스트리 멤버 의도적 제외
struct ConfigureContext {
    Server& server;                    // adapter<T>(role) 접근용
    uint32_t core_id;
    PerCoreState& per_core_state;      // 할당기 등 per-core 리소스
    // ServiceRegistry 없음 → 다른 서비스 접근 시 컴파일 에러
};

// Phase 2: 서비스 간 와이어링 + 유틸리티
struct WireContext {
    Server& server;
    uint32_t core_id;
    ServiceRegistry& local_registry;         // 이 코어의 서비스들
    ServiceRegistryView& global_registry;    // 전 코어의 서비스 (읽기 전용, const 래퍼)
    PeriodicTaskScheduler& scheduler;        // 주기적 작업 등록
};
```

### 3.3 인터페이스

```cpp
class ServiceBaseInterface {
public:
    virtual ~ServiceBaseInterface() = default;
    virtual void on_configure(ConfigureContext& ctx) {}
    virtual void on_wire(WireContext& ctx) {}
    virtual void on_start() {}
    virtual void on_stop() {}
    virtual void on_session_closed(SessionId session_id) {}
    virtual void bind_dispatcher(MessageDispatcher& external) = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual bool started() const noexcept = 0;
};

template <typename Derived>
class ServiceBase : public ServiceBaseInterface {
    PerCoreState* per_core_ = nullptr;

    // 프레임워크가 호출 (on_configure 전)
    void internal_configure(ConfigureContext& ctx) {
        per_core_ = &ctx.per_core_state;
        static_cast<Derived*>(this)->on_configure(ctx);
    }

    // 프레임워크가 호출 (on_start 후)
    void internal_start() {
        static_cast<Derived*>(this)->on_start();
        if (has_kafka_handlers()) {
            setup_kafka_bridge();
        }
    }

protected:
    BumpAllocator& bump() { return per_core_->bump_allocator(); }
    ArenaAllocator& arena() { return per_core_->arena_allocator(); }
    uint32_t core_id() const { return per_core_->core_id(); }

    // TCP 메시지 핸들러 등록 (FlatBuffers 검증 내장)
    template <typename FbsType>
    void route(uint32_t msg_id,
               awaitable<Result<void>> (Derived::*method)(
                   SessionPtr, uint32_t, const FbsType*));

    // Kafka 메시지 핸들러 등록 (envelope 파싱 + FlatBuffers 검증 내장)
    template <typename FbsType>
    void kafka_route(uint32_t msg_id,
                     awaitable<Result<void>> (Derived::*method)(
                         const EnvelopeMetadata&, uint32_t, const FbsType*));
};
```

---

## 4. 어댑터 레지스트리

### 4.1 다중 등록 API

```cpp
// 역할(role) 지정 등록
server.add_adapter<RedisAdapter>("auth", auth_config);
server.add_adapter<RedisAdapter>("pubsub", pubsub_config);
server.add_adapter<RedisAdapter>("ratelimit", rl_config);

// 역할 생략 = "default" (기존 코드 하위 호환)
server.add_adapter<RedisAdapter>(config);

// 접근
auto& redis_auth = server.adapter<RedisAdapter>("auth");
auto& redis = server.adapter<RedisAdapter>();  // role = "default"
```

### 4.2 내부 구현

- 키: `{type_index, role_string}` 쌍
- 컨테이너: 어댑터 수 10개 미만 소규모, 핫패스 아님 — flat 컨테이너 또는 small_vector 중 구현 시 판단
- 역할 생략 시 `"default"` 키 사용으로 기존 API 하위 호환 유지
- **검색 실패 시**: `adapter<T>(role)` — 해당 타입+역할 미등록이면 `std::out_of_range` throw. 서비스 초기화 시 즉시 발견 가능

---

## 5. Per-Core 서비스 레지스트리

### 5.1 API

```cpp
class ServiceRegistry {
public:
    template <typename T>
    T& get();              // 없으면 std::logic_error throw

    template <typename T>
    T* find();             // 없으면 nullptr (안전한 탐색용)
};

// 읽기 전용 래퍼 — 모든 멤버 함수가 const, mutable 멤버 없음
class ServiceRegistryView {
public:
    template <typename T>
    void for_each_core(std::function<void(uint32_t core_id, const T&)> fn) const;

    template <typename T>
    const T& get(uint32_t core_id) const;  // 없으면 std::logic_error throw
};
```

### 5.2 자동 등록

Server가 서비스 인스턴스 생성 시 `type_index`로 자동 키잉하여 레지스트리에 등록.
서비스 코드에서 수동 등록 불필요.

### 5.3 스레드 안전성

- `ServiceRegistry`: per-core 전용, 단일 스레드 접근. 동기화 불필요.
- `ServiceRegistryView`: Phase 2(on_wire) 시점에만 사용. 이 시점에 모든 서비스 인스턴스가 생성 완료 상태이고 코어 스레드 시작 전이므로, 읽기 전용 접근은 데이터 레이스 없이 안전.

---

## 6. 세션 라이프사이클 훅

### 6.1 on_session_closed

```cpp
virtual void on_session_closed(SessionId session_id) {}
```

SessionManager의 타임아웃 콜백 / 연결 종료 시 → 해당 코어의 모든 서비스에 호출.
오버라이드 안 하면 no-op.

### 6.2 Gateway 활용

```cpp
void GatewayService::on_session_closed(SessionId session_id) {
    auth_states_.erase(session_id);
    pending_requests_.remove_by_session(session_id);
}
```

---

## 7. 주기적 작업 스케줄러

### 7.1 API

```cpp
class PeriodicTaskScheduler {
public:
    TaskHandle schedule(std::chrono::milliseconds interval,
                        std::function<void()> task);
    TaskHandle schedule_all_cores(std::chrono::milliseconds interval,
                                  std::function<void(uint32_t core_id)> task);
    void cancel(TaskHandle handle);
};
```

### 7.2 내부 구현

```cpp
// TaskHandle: 등록된 주기적 작업의 식별자 (취소용)
using TaskHandle = uint64_t;  // 자동 증가 카운터, 0 = invalid
```

**동작 방식**:
- 각 코어의 io_context에 `boost::asio::steady_timer` 생성
- 타이머 만료 시 task 실행 → 재스케줄 (재귀 co_spawn)
- `cancel(handle)`: 내부 task map에서 제거 → 다음 타이머 만료 시 재스케줄 안 함
- Server shutdown: 전체 task map clear → steady_timer::cancel() → 코루틴 자연 종료
- `schedule_all_cores()`: 각 코어의 io_context에 독립적으로 동일 task를 co_spawn. 코어 간 동기화 없음 (shared-nothing 원칙 유지)

**타이머 정밀도**: steady_timer 기반이라 OS 스케줄링 지터 있지만, 주기적 작업(sweep, 메트릭 수집 등)에는 충분. CoreEngine의 tick_interval(100ms)과 독립적으로 동작.

WireContext에서 제공:
```cpp
void GatewayService::on_wire(WireContext& ctx) {
    ctx.scheduler.schedule(1s, [this] { sweep_expired_requests(); });
}
```

---

## 8. `route<T>()` FlatBuffers 검증 내장

`route<T>()`가 내부에서:
1. `flatbuffers::Verifier`로 버퍼 구조 검증
2. 실패 시 `ErrorSender::build_error_frame(msg_id, ErrorCode::FlatBuffersVerifyFailed)` 자동 전송 (기존 `apex_core/error_sender.hpp` 활용)
3. 성공 시 `GetRoot<T>()` 후 핸들러 호출

핸들러는 검증 완료된 `const T*`만 받음. 개별 필드 null check는 비즈니스 로직이므로 서비스 책임.

`kafka_route<T>()`도 동일한 FlatBuffers 검증 적용. 에러 처리 차이:
- **TCP `route<T>()`**: 클라이언트 세션에 error frame 전송
- **Kafka `kafka_route<T>()`**: EnvelopeMetadata의 reply_topic으로 error envelope 전송. reply_topic 없으면 에러 로깅 후 메시지 드랍

---

## 9. Kafka 메시지 통합

### 9.1 kafka_route<T>()

```cpp
template <typename FbsType>
void kafka_route(uint32_t msg_id,
                 awaitable<Result<void>> (Derived::*method)(
                     const EnvelopeMetadata&, uint32_t, const FbsType*));
```

핸들러 시그니처가 메시지 소스를 명시:
- `route<T>()` → `SessionPtr` (TCP 클라이언트)
- `kafka_route<T>()` → `const EnvelopeMetadata&` (Kafka 메시지)

### 9.2 자동 브릿지

ServiceBase의 `internal_start()`에서 kafka_route 등록 감지 시 Kafka 어댑터의 message callback을 디스패처에 자동 연결.

### 9.3 current_meta_ 제거

기존: `current_meta_` 멤버에 캐싱 (코루틴 중첩 시 덮어쓰기 위험)
변경: `kafka_route<T>()`가 EnvelopeMetadata를 값으로 핸들러에 전달. 멤버 캐싱 불필요.

### 9.4 EnvelopeMetadata 수명 보장

kafka_route 래퍼 내부에서 EnvelopeMetadata를 **값으로 복사**하여 코루틴 프레임에 캡처.
핸들러가 받는 `const EnvelopeMetadata&`는 이 코루틴 프레임 내 복사본을 참조하므로,
핸들러 실행 기간(co_await 포함) 동안 안전. 다음 Kafka 메시지가 원본을 덮어써도 영향 없음.

```cpp
// kafka_route 래퍼 내부 (프레임워크 코드)
co_spawn(io_ctx, [meta_copy = std::move(meta), ...]() -> awaitable<void> {
    // meta_copy는 코루틴 프레임에 소유됨
    co_await handler(meta_copy, msg_id, fbs_root);  // const& 전달
}, detached);
```

---

## 10. EnvelopeBuilder + Arena 활용

### 10.1 API

```cpp
// apex_shared에 추가
class EnvelopeBuilder {
public:
    EnvelopeBuilder& routing(uint32_t msg_id, uint16_t flags = 0);
    EnvelopeBuilder& metadata(uint16_t core_id, uint64_t corr_id,
                               SourceId source, uint64_t session_id,
                               uint64_t user_id);
    EnvelopeBuilder& reply_topic(std::string_view topic);
    EnvelopeBuilder& payload(std::span<const uint8_t> data);

    // BumpAllocator에 할당 (핫패스 — 힙 할당 없음)
    std::span<uint8_t> build_into(BumpAllocator& alloc);
};
```

### 10.2 할당 전략

- 기본 경로: `build_into(bump())` — zero-alloc, 메시지 처리 후 bump.reset()
- 데이터 보관 필요 시: 서비스 개발자가 명시적으로 `std::vector` 복사
- 힙 할당의 비용이 코드에 드러남 → 성능 의식적 코드 유도

### 10.3 BumpAllocator 접근

ServiceBase의 `internal_configure()`에서 PerCoreState 자동 캐싱.
서비스에서 `bump()`, `arena()` 접근자로 사용.

### 10.4 Lifetime 규칙

`build_into(bump())`로 할당된 데이터의 수명 = 현재 메시지 핸들러의 co_await 전까지.
(기존 FlatBuffers `const T*`의 수명 규칙과 동일)

```cpp
// ✓ 안전: build → produce → 끝 (대부분의 경우)
auto envelope = builder.build_into(bump());
kafka_->produce(topic, envelope);

// ✓ 안전: co_await 전이면 추가 참조 OK
auto envelope = builder.build_into(bump());
log_metadata(envelope);
kafka_->produce(topic, envelope);

// ✗ 위험: 핸들러 밖에서 보관
pending_map[id] = envelope;  // bump.reset() 후 댕글링

// ✓ 안전: 보관 필요 시 명시적 복사
auto copy = std::vector<uint8_t>(envelope.begin(), envelope.end());
pending_map[id] = std::move(copy);
```

---

## 11. Server 리스너 0개 지원

Server.run()은 리스너 0개, 어댑터 0개에도 정상 동작.
CoreEngine 시작 → 라이프사이클 훅 → 시그널 대기 → 정상 종료.
별도 설계 불필요, 기존 흐름에서 리스너/어댑터 관련 루프가 자연스럽게 스킵되면 됨.

---

## 12. 서비스 마이그레이션

### 12.1 Auth 서비스 (CoreEngine → Server + ServiceBase)

변경 전:
- CoreEngine 직접 생성, 어댑터 수동 init, MessageDispatcher 수동 관리
- dispatch_envelope() + current_meta_ 멤버 캐싱
- main.cpp 100줄+

변경 후:
```cpp
class AuthService : public ServiceBase<AuthService> {
    void on_configure(ConfigureContext& ctx) override {
        kafka_ = &ctx.server.adapter<KafkaAdapter>("request");
        kafka_out_ = &ctx.server.adapter<KafkaAdapter>("response");
        redis_ = &ctx.server.adapter<RedisAdapter>();
        pg_ = &ctx.server.adapter<PgAdapter>();
    }

    void on_start() override {
        kafka_route<LoginRequest>(MSG_LOGIN, &AuthService::on_login);
        kafka_route<LogoutRequest>(MSG_LOGOUT, &AuthService::on_logout);
        kafka_route<RefreshTokenRequest>(MSG_REFRESH, &AuthService::on_refresh);
    }

    awaitable<Result<void>> on_login(
        const EnvelopeMetadata& meta, uint32_t msg_id,
        const LoginRequest* req) {
        // 비즈니스 로직만
        auto envelope = EnvelopeBuilder{}
            .routing(MSG_LOGIN_RESPONSE)
            .metadata(core_id(), meta.corr_id, SourceId::AUTH,
                      meta.session_id, meta.user_id)
            .reply_topic(meta.reply_topic)
            .payload(response_fbs)
            .build_into(bump());
        kafka_out_->produce(meta.reply_topic, envelope);
        co_return ok();
    }
};
```

main.cpp: 15줄 (어댑터 등록 + server.run())

### 12.2 Chat 서비스

Auth와 동일한 마이그레이션 패턴.
7개 kafka_route 등록 (CREATE_ROOM, JOIN_ROOM, LEAVE_ROOM, LIST_ROOMS, SEND_MESSAGE, WHISPER, CHAT_HISTORY).

### 12.3 Gateway 리팩토링

변경 전:
- post_init_callback 250줄 거대 람다
- dynamic_cast 4회, null check 없음
- auth_states_ 메모리 누수

변경 후:
```cpp
void GatewayService::on_configure(ConfigureContext& ctx) override {
    kafka_ = &ctx.server.adapter<KafkaAdapter>();
    redis_auth_ = &ctx.server.adapter<RedisAdapter>("auth");
    redis_pubsub_ = &ctx.server.adapter<RedisAdapter>("pubsub");
    redis_rl_ = &ctx.server.adapter<RedisAdapter>("ratelimit");
    // RouteTable, JwtVerifier 등 설정 기반 초기화
}

void GatewayService::on_wire(WireContext& ctx) override {
    // ResponseDispatcher 구성 (per-core pending_requests 수집)
    ctx.global_registry.for_each_core<GatewayService>(
        [&](uint32_t cid, const GatewayService& gw) { /* collect */ });
    // PubSubListener 와이어링
    // RateLimiter 와이어링
    ctx.scheduler.schedule(1s, [this] { sweep_expired(); });
}

void GatewayService::on_start() override {
    set_default_handler(&GatewayService::on_client_message);
}

void GatewayService::on_session_closed(SessionId sid) override {
    auth_states_.erase(sid);
    pending_requests_.remove_by_session(sid);
}
```
- main.cpp: 어댑터 등록 + server.run()만 잔류
- **기존 `post_init_callback`은 이번 PR에서 완전 제거** — 모든 와이어링이 on_configure/on_wire로 이전

---

## 13. 변경 대상 파일

### 코어 프레임워크 (apex_core/)

| 파일 | 변경 |
|------|------|
| `service_base.hpp` | 라이프사이클 훅, kafka_route, bump()/arena(), internal_configure/start |
| `service_base_interface.hpp` | on_configure, on_wire, on_session_closed 가상 메서드 |
| `server.hpp/.cpp` | 어댑터 다중 등록, 서비스 레지스트리, 라이프사이클 오케스트레이션, 리스너 0개 지원 |
| `service_registry.hpp` (신규) | 타입 기반 per-core 레지스트리 |
| `periodic_task_scheduler.hpp` (신규) | 주기적 작업 API |
| `configure_context.hpp` (신규) | Phase 1 Context |
| `wire_context.hpp` (신규) | Phase 2 Context |
| `session_manager.hpp/.cpp` | 세션 종료 시 서비스 훅 호출 |

### 공유 라이브러리 (apex_shared/)

| 파일 | 변경 |
|------|------|
| `envelope_builder.hpp` (신규) | EnvelopeBuilder + build_into(BumpAllocator&) |
| `kafka_dispatch_bridge.hpp` (신규) | Kafka → dispatcher 자동 브릿지 |

### 서비스 (apex_services/)

| 파일 | 변경 |
|------|------|
| `gateway/` 전체 | post_init_callback 해체, on_configure/on_wire/on_start/on_session_closed, main.cpp 축소 |
| `auth-svc/` 전체 | CoreEngine → Server+ServiceBase, kafka_route 전환, current_meta_ 제거, main.cpp 축소 |
| `chat-svc/` 전체 | Auth와 동일 패턴 |

### 테스트

| 파일 | 변경 |
|------|------|
| 기존 유닛 테스트 | ServiceBase 라이프사이클 변경에 맞게 업데이트 |
| 신규 유닛 테스트 | 아래 상세 |
| E2E 테스트 | 기존 11개 시나리오 regression + 신규 커버리지 |

**신규 유닛 테스트 상세**:
- `ServiceRegistry`: 타입 기반 등록/검색, 미등록 타입 `std::logic_error`, `find()` nullptr 반환
- `PeriodicTaskScheduler`: interval 실행 확인, cancel 후 미실행, shutdown 시 자동 정리
- `EnvelopeBuilder`: `build_into()` 메모리 레이아웃, RoutingHeader/MetadataPrefix 오프셋 정확성
- `ConfigureContext/WireContext`: 라이프사이클 페이즈 순서 강제 (Phase 1에서 registry 접근 불가 = 컴파일 에러)
- `kafka_route<T>()`: FlatBuffers 검증 성공/실패, EnvelopeMetadata 파싱 정확성, 잘못된 envelope 처리
- `route<T>()` FlatBuffers 자동 검증: 유효/무효 버퍼, error frame 자동 전송 확인
- 어댑터 다중 등록: 동일 타입 다중 역할 등록/조회, 미등록 역할 `std::out_of_range`

**E2E 신규 커버리지**:
- Auth/Chat 마이그레이션 후 기존 11개 시나리오 정상 동작 (regression)
- `on_session_closed` 훅: 세션 종료 후 auth_states_/pending 정리 확인
- 주기적 작업: sweep 타이머 동작 확인 (timeout된 pending request 정리)

### 기존 서비스 호환성

EchoService 등 기존 테스트용 서비스는 on_start()만 사용하며 on_configure/on_wire를 오버라이드하지 않아도 됨 (default no-op). 기존 서비스는 변경 없이 동작.
