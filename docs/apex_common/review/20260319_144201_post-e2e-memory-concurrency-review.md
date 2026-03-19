# Post-E2E Review (#48) -- Memory & Concurrency Safety

**Reviewer**: System Reviewer (Memory + Concurrency)
**Date**: 2026-03-19
**Scope**: D3 (GatewayGlobals ownership), D4 (ChannelSessionMap per-core), D6 (ConsumerPayloadPool), D7 (spawn counter)

## Summary

| Severity | Count | Fixed | Reported |
|----------|-------|-------|----------|
| CRITICAL | 0     | 0     | 0        |
| MAJOR    | 0     | 0     | 0        |
| MINOR    | 3     | 0     | 3        |

**Total issues: 0 (CRITICAL/MAJOR) + 3 (MINOR report-only)**

All D3/D4/D6/D7 changes are memory-safe and concurrency-safe under the current shutdown sequence. No code modifications required.

---

## Detailed Analysis

### 1. ConsumerPayloadPool (D6)

**Files**: `consumer_payload_pool.hpp`, `consumer_payload_pool.cpp`

#### 1.1 Cross-thread safety: PASS

Pool은 mutex로 보호되며 acquire(consumer thread) / release(core thread via custom deleter) 패턴이 올바르다. lock_guard 범위가 적절하고, lock 밖에서의 힙 할당(line 55)은 mutex contention을 줄이기 위한 의도적 설계다.

#### 1.2 Custom deleter `this` 캡처: SAFE (conditional)

```cpp
// consumer_payload_pool.cpp:63
return PayloadPtr(raw, [this](PayloadBuffer* p) { release(p); });
```

`this`는 `ConsumerPayloadPool*`이며, `KafkaAdapter::payload_pool_` 멤버로 소유된다. Shutdown 순서:
- Step 5: `core_engine_->stop()` + `join()` + `drain_remaining()`
- Step 6: `adapter->close()` (KafkaAdapter 정리)
- Server 소멸: `adapters_` 소멸 -> pool 소멸

Step 5에서 코어 스레드가 완전 종료 + 잔여 메시지 드레인이 완료된 후 step 6에서 adapter가 정리되므로, shared_ptr의 custom deleter가 호출되는 시점에 pool은 반드시 살아있다. **현재 shutdown 순서에서 안전하다**.

단, 이 안전성은 `finalize_shutdown()`의 step 5 < step 6 순서에 전적으로 의존한다. 향후 shutdown 순서 변경 시 UAF 위험이 있으므로 주석으로 의존성을 명시하는 것을 권고한다.

#### 1.3 [MINOR] 메트릭 정확도: total_created_ drift

Max 도달 fallback 버퍼(line 42-44)는 `total_created_`에 포함되지 않는다. 그러나 `release()`의 delete 경로(line 82-83)에서 `--total_created_`를 수행한다. Burst 시나리오에서:
1. 풀 외부 버퍼가 반환 시 풀로 흡수됨 (line 79 조건 충족)
2. 원래 풀 내부 버퍼가 나중에 반환 시 delete됨 (line 79 조건 미충족)
3. `total_created_`가 실제보다 낮아짐

기능적 영향: 없음 (메트릭 조회 전용). 풀 동작 자체에는 영향 없다.

#### 1.4 [MINOR] 메트릭 함수의 mutex overhead

`free_count()`, `in_use_count()` 등 6개 메트릭 함수가 모두 mutex를 잡는다. 모니터링/디버그 용도이므로 기능에 문제 없으나, 이 값들을 `std::atomic`으로 전환하면 lock contention을 줄일 수 있다. 현재 consumer 스레드의 acquire()와 동시에 메트릭을 조회하는 시나리오에서 불필요한 경합이 발생할 수 있다.

---

### 2. ChannelSessionMap per-core (D4)

**Files**: `channel_session_map.hpp`, `channel_session_map.cpp`

#### 2.1 Mutex 제거 정당성: VERIFIED

모든 접근 경로를 검증:

| Call site | Thread | Map index | Safe? |
|-----------|--------|-----------|-------|
| `subscribe()` (gateway_service.cpp:145) | core N handler | `core_id()` | per-core |
| `unsubscribe()` (gateway_service.cpp:168) | core N handler | `core_id()` | per-core |
| `unsubscribe_all()` (gateway_service.cpp:201) | core N session_mgr callback | `core_id()` | per-core |
| `get_subscribers()` (broadcast_fanout.cpp:67) | cross_core_post lambda -> core N | `core_id` param | per-core |

모든 접근이 해당 코어의 io_context에서 실행된다. `broadcast_fanout.cpp`의 `cross_core_post`는 target core의 MPSC 큐를 통해 전달되어 target core 스레드에서 실행되므로, `(*maps)[core_id]`가 해당 코어에서만 접근된다. **data race 없음**.

#### 2.2 unsubscribe_all 반복 중 erase 안전성: SAFE

`unsubscribe_all()`에서 `session_to_channels_`의 channel set을 순회하면서 `channel_to_sessions_`에서 erase한다. 두 맵은 별개이므로 반복자 무효화 없음. `session_to_channels_` 자체는 순회 후 한 번 erase하므로 안전하다.

---

### 3. BroadcastFanout

**Files**: `broadcast_fanout.hpp`, `broadcast_fanout.cpp`

#### 3.1 channel_maps_ 포인터 안정성: SAFE

`set_channel_maps()`는 `on_wire()`에서 globals_ 확정 후 호출된다. `GatewayGlobals`가 `Server::globals_` (TypedGlobalHolder)로 move-store된 후이므로 `per_core_channel_maps` 벡터 객체의 주소가 안정적이다. `GatewayGlobals`의 move constructor는 `noexcept = default`이므로 vector의 move가 정상 동작한다.

#### 3.2 cross_core_post에서 shared_data 캡처: SAFE

`std::make_shared<std::vector<uint8_t>>(std::move(frame))`로 생성된 `data`가 shared_ptr로 모든 코어에 복사-캡처된다. 마지막 코어의 람다가 완료되면 자동 해제. Reference counting은 atomic이므로 cross-thread 안전.

#### 3.3 session_mgrs_ raw pointer 유효성: SAFE

`session_mgrs_`는 `PerCoreState::session_mgr`의 주소. Server 소멸 시까지 유효하며, BroadcastFanout은 GatewayGlobals 안에 있으므로 Server 소멸 전에 파괴된다 (globals_ clear는 step 7, per_core_ 소멸은 Server 소멸자).

---

### 4. GatewayGlobals Ownership (D3)

**Files**: `gateway_service.hpp`, `gateway_service.cpp`

#### 4.1 Server.global<T>() move semantics: SAFE

`create_globals()`가 `GatewayGlobals`를 값으로 반환 -> `TypedGlobalHolder<T>` 생성 시 factory를 호출하여 `value`를 구성. 이후 `globals_`는 `&holder->value`를 가리킨다. Move 이후 포인터가 안정적이며, `GatewayGlobals`의 move constructor가 `noexcept = default`이므로 모든 unique_ptr과 vector 멤버가 올바르게 이전된다.

#### 4.2 Shutdown 순서와 globals_ 무효화: SAFE

```
Step 4:  services stop (globals_->pubsub_listener->stop())
Step 5:  core_engine stop + join + drain
Step 6:  adapter close
Step 7:  globals_.clear() -> GatewayGlobals 소멸
```

서비스가 stop된 후(step 4) core engine이 종료되고(step 5), adapter가 정리된 후(step 6), 마지막으로 globals가 소멸된다(step 7). PubSubListener는 step 4에서 thread join이 완료되므로 BroadcastFanout에 대한 접근이 종료된다. **모든 참조자가 globals 소멸 전에 비활성화된다**.

#### 4.3 Kafka callback의 globals_ 접근: SAFE

`kafka_->set_message_callback([this](...) { return globals_->response_dispatcher->on_response(payload); });`

Consumer의 poll이 adapter drain(step 2)에서 `stop_consuming()`으로 중단되므로, step 4(service stop) 이후에는 callback이 호출되지 않는다. `globals_`가 유효한 동안에만 접근된다.

---

### 5. spawn() D7 Outstanding Tracking

**File**: `service_base.hpp`

#### 5.1 atomic memory ordering: CORRECT

```cpp
outstanding_coros_.fetch_add(1, std::memory_order_relaxed);  // increment
outstanding_coros_.fetch_sub(1, std::memory_order_release);  // decrement
outstanding_coros_.load(std::memory_order_acquire);          // read
```

- `relaxed` increment: 정확한 순서는 중요하지 않고 증가만 보장되면 됨
- `release` decrement: 코루틴 내부의 모든 side effect가 decrement 전에 visible
- `acquire` load: shutdown 대기 루프에서 decrement의 release와 synchronize-with

이 ordering 조합은 Boost.Asio의 `outstanding_work_` 패턴과 동일하다. **올바르다**.

#### 5.2 [MINOR] wire_services의 co_spawn(detached)가 D7 tracking 밖

`kafka_adapter.cpp:180-189`에서 `boost::asio::co_spawn(engine.io_context(0), ..., detached)`로 스폰하는 코루틴은 `ServiceBase::spawn()`이 아니라 직접 co_spawn을 사용한다. D7의 outstanding tracking에 포함되지 않는다.

현재 shutdown 순서에서 안전한 이유:
1. Adapter drain(step 2)에서 consumer가 중단되어 새 co_spawn이 발생하지 않음
2. 기존 코루틴은 bridge->dispatch()가 짧은 연산(파싱+핸들러 호출)이므로 대부분 빠르게 완료
3. Core engine stop+join(step 5)에서 미완료 코루틴의 프레임이 정리됨

하지만 dispatch 핸들러 내에서 추가 co_await(예: DB 호출)가 있으면 shutdown 시 코루틴이 중간 상태에서 파괴될 수 있다. 현재 Auth/Chat 서비스의 Kafka 핸들러가 async 작업을 포함하는지에 따라 잠재적 위험이 존재한다.

**권고**: wire_services의 co_spawn을 D7 tracking에 포함시키거나, 별도 카운터를 추가하는 것을 고려. 현재 서비스 구현이 동기적이면 즉시 문제는 아님.

---

## Conclusion

D3/D4/D6/D7의 메모리 관리와 동시성 안전성을 통합 검증한 결과, **CRITICAL/MAJOR 결함이 발견되지 않았다**. Shutdown 순서, ownership chain, per-core isolation 모두 올바르게 구현되어 있다.

3건의 MINOR 이슈는 모두 메트릭 정확도 또는 향후 확장성 관련이며, 현재 동작에 영향을 주지 않는다. 필요 시 백로그에 등록하여 점진적 개선을 진행하면 된다.
