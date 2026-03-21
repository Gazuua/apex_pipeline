# Batch B: 코어/공유 단위 테스트 소탕 — 설계 문서

**백로그**: #67, #101, #13, #14, #16
**브랜치**: `feature/backlog-67-101-13-14-16-core-shared-unit-tests`
**버전**: v0.5.10.0 시점

---

## 1. 목적

프레임워크 코어(`apex_core`)와 공유 라이브러리(`apex_shared`)의 단위 테스트 갭 5건을 일괄 해소한다.
v0.6 운영 인프라 진입 전 회귀 방지 안전망 확보가 목적.

## 2. 스코프

| 백로그 | 대상 API | 모듈 |
|--------|----------|------|
| #67 | `server.global<T>()`, `ConsumerPayloadPool`, `wire_services()` | core, shared |
| #101 | `ErrorSender::build_error_frame()` service_error_code 라운드트립 | core |
| #13 | `Listener<P>` start/drain/stop 라이프사이클 | core |
| #14 | `WebSocketProtocol` try_decode()/consume_frame() | shared |
| #16 | `PgTransaction` begun_ 경로 (RAII poisoning) | shared |

## 3. 파일 구조

### 3.1 새 파일 (7개)

```
apex_core/tests/unit/
├── test_server_global.cpp              (#67-A) [신규]
├── test_wire_services.cpp              (#67-C) [신규]
├── test_error_sender_service_code.cpp  (#101)  [신규]
└── test_listener_lifecycle.cpp         (#13)   [신규]

apex_shared/tests/unit/
├── test_consumer_payload_pool.cpp      (#67-B) [신규]
├── test_websocket_protocol.cpp         (#14)   [신규]
└── mock_pg_connection.hpp              (#16 Mock) [신규]
```

### 3.2 수정 파일 (4개)

| 파일 | 변경 내용 |
|------|-----------|
| `apex_shared/lib/adapters/pg/include/apex/shared/adapters/pg/pg_transaction.hpp` | PgTransactionT<Conn> 템플릿화 (§4 참조) |
| `apex_shared/lib/adapters/pg/src/pg_transaction.cpp` | 삭제 또는 explicit instantiation만 잔존 |
| `apex_shared/tests/unit/test_pg_transaction.cpp` | 기존 5개 TC (GTEST_SKIP 3개) → Mock 기반 8개 TC로 교체 |
| `apex_core/tests/unit/CMakeLists.txt` | 4개 테스트 등록 |
| `apex_shared/tests/unit/CMakeLists.txt` | 2개 테스트 등록 (ConsumerPayloadPool, WebSocketProtocol). PgTransaction은 기존 등록 유지 |

## 4. PgTransaction 템플릿 리팩터링 (코드 변경)

### 4.1 문제

`PgConnection`에 virtual 메서드가 없고, `PgTransaction`이 구체 타입(`PgConnection&`)을 직접 참조.
상속 기반 Mock 치환 불가 → 코루틴 경로(begin/commit/rollback) 테스트 불가능.
기존 테스트는 begun_=false 경로만 테스트하고 3개 TC가 `GTEST_SKIP()`.

### 4.2 해법: 템플릿화

```cpp
template <typename Conn>
class PgTransactionT
{
  public:
    PgTransactionT(Conn& conn, PgPool& pool);
    ~PgTransactionT();
    // ... 모든 메서드 inline (코루틴)
  private:
    Conn& conn_;
    PgPool& pool_;
    bool begun_{false};
    bool finished_{false};
};

using PgTransaction = PgTransactionT<PgConnection>;
```

- 기존 코드는 `PgTransaction` 별칭으로 변경 없이 동작
- 테스트에서 `PgTransactionT<MockPgConn>` 으로 Mock 주입
- 구현 75줄을 헤더로 이동 (코루틴 메서드는 사실상 템플릿과 동일한 인스턴스화 모델)
- `pg_transaction.cpp`는 explicit instantiation (`template class PgTransactionT<PgConnection>;`)으로 축소하거나, 헤더 전용으로 전환

### 4.3 MockPgConn

`apex_shared/tests/unit/mock_pg_connection.hpp`에 배치 (기존 `apex_shared/tests/` 하위, mocks 디렉토리 없으므로 테스트 디렉토리에 직접 배치):

```cpp
struct MockPgConn
{
    // query_async: 결과 큐에서 순서대로 반환
    std::queue<apex::core::Result<PgResult>> result_queue;

    boost::asio::awaitable<apex::core::Result<PgResult>> query_async(std::string_view sql)
    {
        queries_.push_back(std::string(sql));
        if (fail_all_) co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        auto r = std::move(result_queue.front());
        result_queue.pop();
        co_return r;
    }

    boost::asio::awaitable<apex::core::Result<PgResult>>
    query_params_async(std::string_view sql, std::span<const std::string> /*params*/)
    {
        return query_async(sql); // 파라미터 바인딩은 Mock에서 무시, SQL만 기록
    }

    // mark_poisoned / is_poisoned 추적
    void mark_poisoned() noexcept { poisoned_ = true; }
    bool is_poisoned() const noexcept { return poisoned_; }

    // 검증 API
    const std::vector<std::string>& queries() const { return queries_; }
    void set_fail_queries(bool v) { fail_all_ = v; }

  private:
    std::vector<std::string> queries_;
    bool poisoned_{false};
    bool fail_all_{false};
};
```

`run_coro()` 헬퍼로 코루틴 테스트 실행.

## 5. 테스트 케이스 상세

### 5.1 #67-A: `test_server_global.cpp` (4개)

| 케이스 | 검증 |
|--------|------|
| `FactoryCalledOnce` | 동일 타입 2회 호출 → factory 1회만 실행 |
| `ReturnsSameInstance` | 2회 호출 → 동일 참조 반환 |
| `MultipleTypesIndependent` | 서로 다른 타입 → 각각 독립 인스턴스 |
| `FactoryWithCapture` | 람다 캡처 값 정확 전달 |

`globals_` 맵은 `Server` 내부이므로, 최소 구성 `Server`로 테스트하거나 `globals_` 로직을 직접 검증.
기존 `test_server_multicore.cpp` 패턴 참조.

### 5.2 #67-B: `test_consumer_payload_pool.cpp` (6개)

| 케이스 | 검증 |
|--------|------|
| `AcquireAndRelease` | PayloadPtr 유효 + release 후 free_count 복원 |
| `PoolExhaustion_FallbackAlloc` | initial_count 소진 → fallback_alloc_count 증가 |
| `MetricsAccuracy` | free/in_use/peak_in_use/total_created 정합성 |
| `MaxCountRespected` | max_count 도달 후 fallback 동작 (크래시 없음) |
| `PayloadDataIntegrity` | acquire 시 span 데이터 → payload() 정확 복사 |
| `ReleaseOrderIndependent` | LIFO/FIFO 무관 올바른 반환 |

CMake 등록: `apex_shared_add_kafka_test()` 사용 (Kafka 어댑터 라이브러리 링크 필요, 60초 타임아웃).

### 5.3 #67-C: `test_wire_services.cpp` (2개)

| 케이스 | 검증 |
|--------|------|
| `DefaultImplementation_NoOp` | `AdapterInterface` 기본 구현 → services 벡터 변경 없음 |
| `CustomAdapter_ModifiesServices` | MockAdapter override → services 벡터 항목 추가 |

`wire_services()`는 `AdapterInterface`의 virtual 메서드이며 기본 구현이 no-op.
테스트는 인터페이스 계약 검증에 집중. 실제 `KafkaAdapter::wire_services()` 통합 흐름은 별도 통합 테스트 영역.

### 5.4 #101: `test_error_sender_service_code.cpp` (4개)

| 케이스 | 검증 |
|--------|------|
| `ServiceErrorCodeZero_Default` | service_error_code=0 → FlatBuffers 필드 0 |
| `ServiceErrorCodeNonZero_RoundTrip` | 1234 → 직렬화/역직렬화 일치 |
| `EmptyMessage_WithServiceCode` | message="" + code>0 → 둘 다 정상 |
| `AllFieldsCombined` | msg_id + ErrorCode + message + service_error_code 전부 라운드트립 |

**라운드트립 검증 방법**: `build_error_frame()` 호출 → `WireHeader::parse()` 로 헤더 추출 → body 오프셋 계산 → `flatbuffers::GetRoot<apex::messages::ErrorResponse>()` 로 역직렬화 → 필드 비교.
기존 `test_error_propagation.cpp:BuildErrorFrame` TC가 동일 패턴 사용 중.
`#include "error_response_generated.h"` 필요.

### 5.5 #13: `test_listener_lifecycle.cpp` (7개)

| 케이스 | 검증 |
|--------|------|
| `StartBindsPort` | start() → port() != 0 |
| `DoubleStartSafe` | start() 2회 → 크래시/예외 없음 |
| `DrainStopsAccepting` | drain() 후 새 연결 거부 |
| `StopAfterDrain` | drain() → stop() 정상 순서 |
| `StopWithoutDrain` | stop() 직접 호출 안전 |
| `ActiveSessionsTracking` | 연결 → active_sessions() 증가/감소 |
| `DispatcherPerCore` | dispatcher(0) ≠ dispatcher(1) |

**Fixture 셋업**: `TEST_F` 사용. `TcpBinaryProtocol`로 `Listener` 인스턴스화.
`CoreEngine` (최소 2코어) + `SessionManager` 벡터 구성 필요.
기존 `test_server_multicore.cpp` 의 `MultiCoreServerTest` fixture가 `CoreEngine` + `Server` 셋업 패턴을 제공.
`test_tcp_acceptor.cpp`의 TCP 연결 패턴(`make_socket_pair()` 또는 직접 `tcp::socket::connect()`) 참조.

### 5.6 #14: `test_websocket_protocol.cpp` (7개)

| 케이스 | 검증 |
|--------|------|
| `DecodeCompleteMessage` | 4바이트 LE 길이 + payload → Frame 추출 |
| `DecodePartialHeader` | 4바이트 미만 → InsufficientData 에러 |
| `DecodePartialPayload` | 헤더 있으나 payload 부족 → InsufficientData |
| `ConsumeAdvancesBuffer` | consume_frame() → RingBuffer read 포인터 전진, writable 증가 |
| `EmptyPayload` | 길이=0 → 빈 Frame 정상 |
| `MultipleFramesInBuffer` | 연속 2메시지 순차 추출 |
| `MaxMessageSizeExceeded` | 초과 길이 → InvalidMessage 에러 |

CMake 등록: WebSocket 프로토콜 전용 함수가 없으므로 수동 `add_executable` + `target_link_libraries(... apex::shared::protocols::websocket ...)` 사용.

### 5.7 #16: `test_pg_transaction.cpp` (8개) — 기존 파일 교체

기존 TC 5개 (GTEST_SKIP 3개 포함) → MockPgConn 기반 8개로 교체.

| 케이스 | 검증 |
|--------|------|
| `BeginSuccess_SetsBegun` | begin() 성공 → execute/commit 가능 |
| `CommitWithoutBegin_ReturnsError` | begin() 미호출 → commit() = AdapterError |
| `RollbackWithoutBegin_ReturnsError` | begin() 미호출 → rollback() = AdapterError |
| `CommitSuccess_SetsFinished` | commit() 성공 → 소멸 시 mark_poisoned 미호출 |
| `RollbackAlwaysSetsFinished` | rollback() 실패해도 finished=true |
| `DestructorPoisons_UnfinishedTxn` | begin() → 소멸 → mark_poisoned() 호출 |
| `DestructorSafe_NotBegun` | begin() 미호출 → 소멸 → mark_poisoned 미호출 |
| `ExecuteAfterCommit_ReturnsError` | commit 후 execute → 에러 |

`PgTransactionT<MockPgConn>` + `run_coro()` 헬퍼 사용.
기존 `NonCopyableNonMovable`, `MultipleTransactionsOnSameConnection` TC는 유지하되 MockPgConn 기반으로 전환.

## 6. CMake 등록 상세

### apex_core/tests/unit/CMakeLists.txt

```cmake
# Batch B: 코어 단위 테스트 (#67-A, #67-C, #101, #13)
apex_add_unit_test(test_server_global test_server_global.cpp)
apex_add_unit_test(test_wire_services test_wire_services.cpp)
apex_add_unit_test(test_error_sender_service_code test_error_sender_service_code.cpp)
apex_add_unit_test(test_listener_lifecycle test_listener_lifecycle.cpp)
```

`set_tests_properties` 블록에 4개 테스트명 추가 (TIMEOUT 30).

### apex_shared/tests/unit/CMakeLists.txt

```cmake
# ConsumerPayloadPool — Kafka 라이브러리 링크 필요
apex_shared_add_kafka_test(test_consumer_payload_pool test_consumer_payload_pool.cpp)

# WebSocketProtocol — 전용 함수 없으므로 수동 등록
add_executable(test_websocket_protocol test_websocket_protocol.cpp)
target_link_libraries(test_websocket_protocol
    PRIVATE
        apex::shared::protocols::websocket
        GTest::gtest_main
)
apex_set_warnings(test_websocket_protocol)
add_test(NAME test_websocket_protocol COMMAND test_websocket_protocol)
set_tests_properties(test_websocket_protocol PROPERTIES TIMEOUT 30)
```

`test_pg_transaction`은 기존 `apex_shared_add_pg_test` 등록 유지 (변경 없음).

## 7. 기존 패턴 준수

- **GTest**: `TEST()` / `TEST_F()` 사용
- **헬퍼**: `test_helpers.hpp` — `run_coro()`, `wait_for()`, `make_socket_pair()`
- **타임아웃**: 30초 기본, Kafka 관련 60초 (TSAN/ASAN 10배 확대)
- **경고**: `apex_set_warnings()` 적용
- **저작권 헤더**: MIT 라이선스 헤더 필수

## 8. 총계

- **새 파일**: 7개 (테스트 6 + mock 1)
- **수정 파일**: 5개 (pg_transaction.hpp 리팩터링, pg_transaction.cpp 축소, test_pg_transaction.cpp 교체, CMakeLists.txt × 2)
- **테스트 케이스**: 38개
- **프로덕션 코드 변경**: PgTransaction 템플릿화 1건 (기능 변경 없음, API 호환)
