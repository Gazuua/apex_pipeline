# 테스트 커버리지 보강 — CORE+SHARED 유닛 테스트 설계

- **백로그**: BACKLOG-148, BACKLOG-191, BACKLOG-192
- **브랜치**: `feature/test-coverage-core-shared`
- **스코프**: apex_core, apex_shared
- **작성일**: 2026-03-24

## 1. 목적

auto-review에서 식별된 핵심 경로 미테스트 9건(BACKLOG-148) + MetricsHttpServer 전체(BACKLOG-191) + 어댑터 init 실패/복구 경로(BACKLOG-192)를 유닛 테스트로 커버한다.

## 2. 소스 코드 변경 (테스트 지원)

테스트 작성에 필요한 최소한의 소스 코드 변경:

| 변경 | 이유 |
|------|------|
| **AdapterBase::init()** — do_init() 예외 시 state를 CLOSED로 롤백하는 try-catch 추가 | 현재 do_init() 예외 시 state가 RUNNING으로 남아 상태 일관성이 깨짐. BACKLOG-148① "init 실패 복구" 테스트의 전제 조건 |
| **MetricsHttpServer** — `uint16_t local_port() const` 접근자 추가 | port 0 바인딩 테스트에서 OS 할당 포트를 얻기 위해 필요. acceptor_가 private이므로 접근자 필수 |

## 3. 파일 구성

### 기존 파일 확장 (7개)

| 파일 | 추가 항목 |
|------|-----------|
| `apex_shared/tests/unit/test_circuit_breaker.cpp` | `CircuitBreakerConcurrencyTest` 픽스처 — 코루틴 동시 call(), HALF_OPEN throttling |
| `apex_core/tests/unit/test_cross_core_call.cpp` | 동일 코어 호출(src==dst) 타임아웃 검증 |
| `apex_core/tests/unit/test_session.cpp` | `SessionConcurrencyTest` 픽스처 — 동시 async_send(), enqueue_write max depth |
| `apex_core/tests/unit/test_timing_wheel.cpp` | num_slots=1 생성, 단일 슬롯 다중 엔트리 만료 순서 |
| `apex_core/tests/unit/test_frame_codec.cpp` | UnsupportedProtocolVersion 에러코드 반환 검증 |
| `apex_core/tests/unit/test_config.cpp` | num_cores=0 Config 로드 동작 검증 |
| `apex_core/tests/unit/test_spsc_mesh.cpp` | `SpscMeshDeathTest` 픽스처 — queue(0,0) assert, NDEBUG 가드 |

### 신규 파일 (3개)

| 파일 | 내용 |
|------|------|
| `apex_core/tests/unit/test_metrics_http_server.cpp` | port 0 바인딩 + raw TCP 요청으로 /metrics, /health, /ready, 404 검증 |
| `apex_shared/tests/unit/test_redis_multiplexer_close.cpp` | close() 중 pending command 콜백 안전성, close 후 command 거부 |
| `apex_shared/tests/unit/test_adapter_base_failure.cpp` | mock 서브클래스로 do_init() 예외 시 상태 롤백, 이중 init, close 타임아웃 |

## 4. 테스트 케이스 상세 (총 22건)

### A. 동시성 테스트 (6건)

#### CircuitBreakerConcurrencyTest (apex_shared)

CircuitBreaker의 상태 변수(state_, failure_count_ 등)는 비atomic — 단일 코어 내 코루틴 안전만 보장하는 설계. 따라서 동시성 테스트는 **단일 io_context에서 다수 코루틴을 co_spawn**하여 코루틴 수준 인터리빙을 검증한다.

| 케이스 | 검증 내용 |
|--------|-----------|
| `ConcurrentCoroCallsCountConsistent` | 단일 io_context에 4개 코루틴 co_spawn → failure_count가 실제 실패 횟수와 일치 |
| `HalfOpenThrottlingUnderConcurrency` | HALF_OPEN 상태에서 동시 co_spawn call()이 half_open_max_calls 초과 시 CircuitOpen 반환 |

#### SessionConcurrencyTest

| 케이스 | 검증 내용 |
|--------|-----------|
| `ConcurrentAsyncSendAllDelivered` | 2개 코루틴 동시 async_send() → write_queue에 모두 도착 (유실 없음) |
| `EnqueueWriteMaxDepthReturnsBufferFull` | max_queue_depth 초과 시 enqueue_write()가 BufferFull 반환 |

#### RedisMultiplexer close 안전성

| 케이스 | 검증 내용 |
|--------|-----------|
| `CloseWithPendingCommandsReturnsError` | pending command 있는 상태에서 close() → 콜백이 AdapterError로 완료 |
| `CommandAfterCloseReturnsError` | close() 후 command() 호출 → 즉시 에러 반환 (use-after-free 없음) |

### B. 에러/엣지케이스 (10건)

#### CrossCoreCall

동일 코어 호출(src==dst) 시 현재 구현은 검사 없이 post → 데드락 → 타임아웃 반환. 테스트는 짧은 타임아웃으로 이 동작을 검증한다.

| 케이스 | 검증 내용 |
|--------|-----------|
| `SameCoreCallReturnsTimeout` | cross_core_call(engine, current_core, fn, 100ms) → CrossCoreTimeout 반환 |

#### TimingWheel

| 케이스 | 검증 내용 |
|--------|-----------|
| `SingleSlotCreation` | num_slots=1 생성 후 정상 동작 (power-of-2 처리 검증) |
| `SingleSlotMultipleEntries` | 슬롯 1개에 다중 엔트리 스케줄 → tick() 시 전부 만료 |

#### FrameCodec

| 케이스 | 검증 내용 |
|--------|-----------|
| `UnsupportedProtocolVersionReturnsError` | 미지원 version 헤더 → ErrorCode::UnsupportedProtocolVersion 반환 |

#### Config

Config 로드 자체는 num_cores=0을 그대로 통과시킨다 (유효성 검증 없음). CoreEngine 생성자에서 hardware_concurrency()로 보정하는 것이 현재 동작.

| 케이스 | 검증 내용 |
|--------|-----------|
| `NumCoresZeroPassesConfigLoad` | num_cores=0 TOML → Config 로드 성공, 값 0 저장 확인 |

#### SpscMeshDeathTest

| 케이스 | 검증 내용 |
|--------|-----------|
| `SelfCoreQueueAsserts` | queue(0,0) → assert 발동 (Debug), NDEBUG시 GTEST_SKIP |

#### AdapterBaseFailureTest

| 케이스 | 검증 내용 |
|--------|-----------|
| `InitFailureRollsBackToClosed` | do_init() 예외 → state가 CLOSED로 롤백 (§2 소스 변경 전제) |
| `DoubleInitWarns` | RUNNING 상태에서 init() 재호출 → 경고 + 무시 |
| `CloseTimeoutLogsWarning` | drain이 5초 초과 → 타임아웃 경고 로그 |
| `RecoveryAfterInitFailure` | CLOSED(롤백 후) → 재init → RUNNING 전이 성공 |

### C. 통합 테스트 (6건)

#### MetricsHttpServerTest

| 케이스 | 검증 내용 |
|--------|-----------|
| `MetricsEndpointReturnsPrometheusFormat` | GET /metrics → 200 + text/plain + Prometheus 직렬화 |
| `HealthEndpointReturnsOk` | GET /health → 200 |
| `ReadyEndpointReflectsRunningFlag` | running=true → 200, running=false → 503 |
| `UnknownPathReturns404` | GET /invalid → 404 |
| `UnsupportedMethodReturns404` | POST /metrics → 404 (현재 서버 동작: 메서드 미분기, else→404) |
| `ConcurrentConnections` | 5개 동시 TCP 연결 → 모두 정상 응답 |

## 5. 기술 구현 세부사항

### 코루틴 동시성 테스트 패턴 (CircuitBreaker, Session)

CircuitBreaker 상태 변수가 비atomic이므로 멀티스레드가 아닌 **단일 io_context + 다중 co_spawn** 패턴을 사용한다.

```cpp
boost::asio::io_context io_ctx;
std::atomic<int> completed{0};

for (int i = 0; i < 4; ++i) {
    boost::asio::co_spawn(io_ctx, [&]() -> boost::asio::awaitable<void> {
        auto result = co_await breaker.call([&]() -> boost::asio::awaitable<Result<int>> {
            co_return std::unexpected(ErrorCode::ConnectionFailed);
        });
        completed.fetch_add(1);
    }, boost::asio::detached);
}

io_ctx.run();
EXPECT_EQ(completed.load(), 4);
EXPECT_EQ(breaker.failure_count(), 4);
```

### 멀티스레드 동시성 패턴 (Session enqueue_write 등)

스레드 안전이 보장되는 API에 대해서만 std::barrier + jthread 패턴 사용:

```cpp
constexpr int N = 4;
std::barrier sync_point(N);
std::vector<std::jthread> threads;
for (int i = 0; i < N; ++i) {
    threads.emplace_back([&] {
        sync_point.arrive_and_wait();
        session->enqueue_write(make_payload(i));
    });
}
// jthread 소멸자가 자동 join → 이후 EXPECT 검증
```

### Death Test 설정

```cpp
class SpscMeshDeathTest : public ::testing::Test {
protected:
    void SetUp() override {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

TEST_F(SpscMeshDeathTest, SelfCoreQueueAsserts) {
#ifdef NDEBUG
    GTEST_SKIP() << "assert disabled in Release";
#else
    // SpscMesh(num_cores, queue_capacity, core_io_contexts) — 3개 인자 필수
    std::vector<boost::asio::io_context> io_ctxs(2);
    std::vector<boost::asio::io_context*> ptrs;
    for (auto& c : io_ctxs) ptrs.push_back(&c);
    SpscMesh mesh(2, 64, ptrs);
    EXPECT_DEATH(mesh.queue(0, 0), "");
#endif
}
```

### MetricsHttpServer 테스트 헬퍼

```cpp
// port 0 바인딩 → OS 할당 포트 취득 (§2의 local_port() 접근자 사용)
void SetUp() override {
    server_.start(io_ctx_, 0, registry_, running_);
    port_ = server_.local_port();
}

// raw TCP HTTP 요청
std::string http_request(const std::string& method, const std::string& path) {
    tcp::socket sock(io_ctx_);
    sock.connect({tcp::v4(), port_});
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    boost::asio::write(sock, boost::asio::buffer(req));
    // 응답 읽기 + 반환
}
```

### AdapterBase Mock 서브클래스

```cpp
class FailingAdapter : public AdapterBase<FailingAdapter> {
    friend class AdapterBase<FailingAdapter>;
    bool should_fail_ = false;
    std::string_view do_name() const noexcept { return "FailingAdapter"; }
    void do_init(CoreEngine& engine) {
        if (should_fail_) throw std::runtime_error("mock init failure");
    }
    void do_drain() { /* no-op */ }
    void do_close() { /* no-op */ }
    void do_close_per_core(uint32_t) { /* no-op */ }
public:
    void set_fail(bool f) { should_fail_ = f; }
};
```

**주의**: AdapterBase는 CRTP 템플릿이므로 `AdapterBase<FailingAdapter>`로 상속.

### CMake 변경

- `apex_core/tests/unit/CMakeLists.txt` — `apex_add_unit_test(test_metrics_http_server ...)` 추가
- `apex_shared/tests/unit/CMakeLists.txt` — `apex_add_unit_test(test_redis_multiplexer_close ...)`, `apex_add_unit_test(test_adapter_base_failure ...)` 추가

## 6. 설계 판단 근거

| 판단 | 근거 |
|------|------|
| MetricsHttpServer: port 0 + raw TCP | 업계 표준, 포트 충돌 원천 차단, 별도 HTTP 라이브러리 불필요 |
| 어댑터 실패: mock 서브클래스 (CRTP) | 실제 연결은 E2E가 커버, 유닛은 상태 전이 로직에 집중 |
| AdapterBase init 롤백 추가 | do_init() 예외 시 상태 일관성 깨짐은 잠재 버그 — 테스트와 함께 수정 |
| Death test: threadsafe + NDEBUG 가드 | Release에서 assert 비활성화 대응, Windows/Linux 크로스플랫폼 안정성 |
| CircuitBreaker: co_spawn (스레드 아님) | 비atomic 상태 변수 → 단일 코어 코루틴 설계 존중, TSAN false positive 방지 |
| POST /metrics → 404 (405 아님) | 현재 서버 동작 기준으로 테스트, 불필요한 서버 코드 변경 회피 |
| num_cores=0: Config 로드 성공 검증 | 실제 동작: Config는 통과, CoreEngine이 보정. 테스트는 현재 계약 기준 |
| CrossCoreCall: 100ms 타임아웃 | 동일 코어 호출은 데드락 → 짧은 타임아웃으로 빠른 검증 |
