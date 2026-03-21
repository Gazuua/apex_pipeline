# 서비스 테스트 작성 가이드

Apex Pipeline의 테스트 패턴과 인프라를 설명한다. 유닛 테스트(GTest + Mock 어댑터)와 E2E 테스트 두 계층을 다룬다.

---

## 1. 테스트 구조 개요

| 계층 | 위치 | 대상 | 실행 |
|------|------|------|------|
| **유닛 테스트** | `apex_core/tests/unit/`, `apex_shared/tests/unit/`, `apex_services/*/tests/` | 클래스·함수 단위 | `build.sh debug` (CTest, `-LE e2e`) |
| **E2E 테스트** | `apex_services/tests/e2e/` | 전체 파이프라인 (Client → Gateway → Kafka → Service → Response) | Docker 인프라 기동 후 별도 실행 |

---

## 2. 유닛 테스트

### 2.1 코루틴 테스트 헬퍼

`apex_core/tests/test_helpers.hpp`에 두 가지 핵심 유틸리티가 정의되어 있다.

**`run_coro()`** — 코루틴을 동기적으로 실행하여 결과를 반환:

```cpp
#include "test_helpers.hpp"

TEST(MyTest, CoroutineReturnsValue)
{
    boost::asio::io_context io_ctx;
    auto result = apex::test::run_coro(io_ctx, my_coroutine());
    EXPECT_TRUE(result.has_value());
}
```

내부적으로 `co_spawn()` + `use_future`로 감싸고, `io_ctx.run()` → `restart()`를 호출한다.

**`wait_for()`** — 비동기 조건을 폴링으로 대기:

```cpp
ASSERT_TRUE(apex::test::wait_for([&]() { return engine.running(); }));
```

기본 타임아웃 3초. ASAN/TSAN 빌드에서는 자동으로 10배 확장된다.

### 2.2 서비스 테스트 패턴

서비스를 테스트할 때는 `ServiceBase<T>`를 상속한 테스트용 서비스를 만들고, `dispatcher().dispatch()`를 직접 호출한다.

```cpp
class EchoService : public ServiceBase<EchoService>
{
  public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override
    {
        handle(0x0001, &EchoService::on_echo);
    }

    awaitable<Result<void>> on_echo(SessionPtr, uint32_t msg_id, std::span<const uint8_t> payload)
    {
        last_msg_id = msg_id;
        last_payload.assign(payload.begin(), payload.end());
        co_return ok();
    }

    // 테스트 검증용 — 내부 상태를 public으로 노출
    uint32_t last_msg_id = 0;
    std::vector<uint8_t> last_payload;
};

TEST(ServiceBase, HandleRegistersAndDispatches)
{
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<EchoService>();
    svc->start();

    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0001, data));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(svc->last_msg_id, 0x0001);
}
```

**핵심 원칙:**
- 핸들러 시그니처: `awaitable<Result<void>> handler(SessionPtr, uint32_t msg_id, std::span<const uint8_t> payload)`
- `on_start()`에서 `handle(msg_id, &Class::method)`로 핸들러 등록
- 서버를 띄우지 않고 디스패처를 직접 호출하여 격리된 테스트 가능
- 검증이 필요한 상태는 public 멤버로 노출

### 2.3 어댑터 수명주기 테스트

모든 어댑터는 동일한 수명주기를 따른다: `constructor → init(engine) → is_ready() → drain() → close()`.

```cpp
TEST(RedisAdapter, FullLifecycle)
{
    RedisConfig config{.host = "localhost", .port = 6379, /* ... */};
    CoreEngine engine(engine_config);

    RedisAdapter adapter(config);
    EXPECT_FALSE(adapter.is_ready());

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());

    adapter.close();
}
```

**테스트 포인트:**
- `init()` 전후 `is_ready()` 상태 전이
- per-core 리소스 접근: `adapter.multiplexer(core_id)`, `adapter.pool(core_id)`
- 에러 경로: `init()` 없이 `drain()`, 이중 `init()` 등

### 2.4 Mock 어댑터

실제 인프라 없이 서비스 핸들러를 테스트하기 위한 Mock이 `apex_services/tests/mocks/`에 준비되어 있다.

| Mock | 파일 | 용도 |
|------|------|------|
| `MockKafkaAdapter` | `mock_kafka_adapter.hpp` | produce 기록 + consume 주입 |
| `MockPgPool` | `mock_pg_pool.hpp` | SQL 쿼리 기록 + 결과 주입 |
| `MockRedisMultiplexer` | `mock_redis_multiplexer.hpp` | 커맨드 기록 + 응답 주입 |

**MockKafkaAdapter 사용 예:**

```cpp
apex::test::MockKafkaAdapter mock;

// 실패 시뮬레이션
mock.set_fail_produce(true);
auto result = mock.produce("topic", "key", payload);
EXPECT_FALSE(result.has_value());

// 메시지 주입 (consume 시뮬레이션)
mock.set_message_callback([&](auto topic, auto partition, auto key, auto payload, auto offset) {
    // 핸들러가 이 콜백을 통해 메시지를 수신
    received = true;
    return apex::core::ok();
});
mock.inject_message("auth.requests", 0, {}, envelope_bytes);

// produce 검증
mock.set_fail_produce(false);
mock.produce("responses", "key", data);
EXPECT_EQ(mock.produce_count(), 1);
EXPECT_EQ(mock.produced()[0].topic, "responses");
```

**Mock 설계 원칙:**
- 실제 어댑터와 동일한 메서드 시그니처
- 모든 호출을 기록하여 사후 검증 가능
- `set_fail_*()` 로 실패 경로 테스트
- 스레드 안전 (내부 mutex)

### 2.5 Envelope 빌드 헬퍼

서비스 간 Kafka 메시지를 테스트할 때는 Envelope(RoutingHeader + MetadataPrefix + FlatBuffers payload)을 조립해야 한다.

```cpp
std::vector<uint8_t> build_envelope(uint32_t msg_id, uint64_t corr_id,
                                    uint16_t core_id, uint64_t session_id,
                                    std::span<const uint8_t> fbs_payload = {})
{
    envelope::RoutingHeader rh;
    rh.msg_id = msg_id;
    // ... 필드 설정

    envelope::MetadataPrefix meta;
    meta.core_id = core_id;
    meta.corr_id = corr_id;
    meta.session_id = session_id;
    // ... 필드 설정

    auto rh_bytes = rh.serialize();
    auto meta_bytes = meta.serialize();

    std::vector<uint8_t> buf;
    buf.reserve(envelope::ENVELOPE_HEADER_SIZE + fbs_payload.size());
    buf.insert(buf.end(), rh_bytes.begin(), rh_bytes.end());
    buf.insert(buf.end(), meta_bytes.begin(), meta_bytes.end());
    buf.insert(buf.end(), fbs_payload.begin(), fbs_payload.end());
    return buf;
}
```

이 헬퍼로 MockKafkaAdapter에 주입할 메시지를 만들 수 있다.

---

## 3. E2E 테스트

### 3.1 인프라 기동

E2E 테스트는 Docker로 전체 인프라를 기동한 뒤 실행한다.

```bash
# 인프라 기동
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --wait

# E2E 테스트 실행
./build/Linux/debug/apex_e2e_tests

# 정리
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

상세 트러블슈팅: `apex_services/tests/e2e/CLAUDE.md` 참조.

### 3.2 테스트 Fixture 구조

E2E 테스트는 2계층 fixture를 사용한다:

- **`E2EEnvironment`** (`::testing::Environment`) — 전체 테스트 스위트에서 1회 실행. Gateway TCP 연결 확인 + Kafka 파이프라인 워밍업 (실제 login 요청/응답으로 검증).
- **`E2ETestFixture`** (`::testing::Test`) — 테스트별 실행. 인프라 준비 상태 확인 + `TcpClient` 제공.

```cpp
class MyE2ETest : public E2ETestFixture
{};

TEST_F(MyE2ETest, LoginAndListRooms)
{
    TcpClient client(io_ctx_, config_);
    client.connect();

    // 1. 로그인
    auto auth = login(client, "alice@apex.dev", "password123");
    ASSERT_FALSE(auth.access_token.empty());

    // 2. JWT 바인딩
    authenticate(client, auth.access_token);

    // 3. 요청 전송 (FlatBuffers)
    flatbuffers::FlatBufferBuilder fbb(128);
    auto req = chat_fbs::CreateListRoomsRequest(fbb, 0, 20);
    fbb.Finish(req);
    client.send(2007, fbb.GetBufferPointer(), fbb.GetSize());

    // 4. 응답 검증
    auto resp = client.recv();
    EXPECT_EQ(resp.msg_id, 2008u);
}
```

### 3.3 제공되는 헬퍼 메서드

`E2ETestFixture`가 제공하는 공통 플로우:

| 메서드 | 설명 |
|--------|------|
| `login(client, email, password)` | 로그인 → `AuthResult` (access_token, refresh_token, user_id) 반환 |
| `authenticate(client, token)` | JWT를 세션에 바인딩 |
| `subscribe_channel(client, channel)` | PubSub 채널 구독 |
| `unsubscribe_channel(client, channel)` | PubSub 채널 구독 해제 |

### 3.4 주의사항

- **컨테이너 볼륨**: 이전 실행의 데이터가 남으면 테스트 실패. `down -v`로 정리 필수.
- **Rate Limit**: PerIpRateLimit 테스트 간 간섭 가능. 윈도우 설정(2초) + 테스트 종료 시 대기.
- **PubSub 타이밍**: 구독 후 메시지 수신까지 지연 존재. `subscribe_channel()` 후 1500ms 대기.
- **Kafka 워밍업**: 컨테이너 healthy ≠ 파이프라인 ready. 실제 요청/응답으로 확인해야 한다.

---

## 4. CMake 테스트 등록

### 4.1 유닛 테스트

```cmake
# apex_core/tests/unit/CMakeLists.txt 패턴
function(apex_add_unit_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE apex::core GTest::gtest_main
    )
    apex_set_warnings(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

apex_add_unit_test(test_my_feature test_my_feature.cpp)

set_tests_properties(test_my_feature PROPERTIES TIMEOUT 30)
```

**필수 규칙:**
- `apex_set_warnings()` 호출 필수 (Zero Warning 정책)
- 타임아웃 30초 (유닛), 120초 (E2E), 300초 (스트레스)

### 4.2 E2E 테스트

```cmake
gtest_discover_tests(apex_e2e_tests
    PROPERTIES LABELS "e2e" TIMEOUT 120
    DISCOVERY_TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
```

`-LE e2e` 플래그로 일반 빌드에서 제외되며, Docker 인프라 기동 후 별도 실행한다.

---

## 5. 새 테스트 추가 체크리스트

1. **테스트 파일 생성**: 해당 모듈의 `tests/unit/` 디렉토리에 `test_<feature>.cpp`
2. **CMake 등록**: `apex_add_unit_test()` 또는 수동 등록 + `apex_set_warnings()` + TIMEOUT
3. **코루틴 코드 테스트 시**: `test_helpers.hpp` include → `run_coro()` / `wait_for()` 사용
4. **서비스 핸들러 테스트 시**: Mock 어댑터 + Envelope 빌드 헬퍼 조합
5. **빌드 확인**: `build.sh debug` (또는 `build.bat`) — CTest가 자동 실행
6. **Sanitizer 확인**: `build.sh asan` / `build.sh tsan`으로 메모리/동시성 이슈 검증
