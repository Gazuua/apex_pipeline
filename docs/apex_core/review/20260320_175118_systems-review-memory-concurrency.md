# Systems Review: Memory + Concurrency (Full Codebase)

**날짜**: 2026-03-20 17:51:18
**리뷰어**: systems (memory + concurrency unified)
**대상**: apex_core, apex_shared/adapters, apex_services
**버전**: v0.5.8.1 (main @ 72a7b77)

---

## 요약

메모리 관리(RAII, lifetime, 커스텀 할당기)와 동시성(코루틴 안전성, strand, 데이터 레이스) 통합 검증 수행.

**결과**: CRITICAL 0건 / MAJOR 1건 / MINOR 4건

전체적으로 코드 품질이 높다. shared-nothing per-core 아키텍처가 잘 설계되어 있어 동시성 이슈의 표면적이 작고, 커스텀 할당기들의 정합성도 양호하다. cross-core 통신 경로의 memory ordering도 적절하다.

---

## 검증 항목별 상세

### 1. RAII 준수 / 리소스 누수 경로

**결론: 양호**

- `Session::~Session()` -> `close()` 호출 (idempotent)
- `SlabAllocator::~SlabAllocator()` -> 모든 chunk `_aligned_free`/`free`
- `RingBuffer::~RingBuffer()` -> `buffer_`와 `linear_buf_` 각각 올바른 free 함수 사용 (`_aligned_free` vs `std::free`)
- `ArenaAllocator::~ArenaAllocator()` -> 모든 block `free`
- `BumpAllocator::~BumpAllocator()` -> `std::free(base_)`
- `TimingWheel::~TimingWheel()` -> 모든 entry `delete`
- `CoreEngine::~CoreEngine()` -> `stop()` + `join()` + `drain_remaining()` (LegacyCrossCoreFn 힙 릭 방지)
- `RedisMultiplexer::~RedisMultiplexer()` -> `cancel_all_pending()` (모든 pending command 정리)

예외 경로:
- `TypedSlabAllocator::construct()` -> 생성자 예외 시 `pool_.deallocate(p)` (올바름)
- `SlabAllocator::grow()` -> `bad_alloc` throw 시 chunks_는 이전 상태 유지 (올바름)
- `ArenaAllocator::allocate()` -> 새 블록 할당 실패 시 `nullptr` 반환 (올바름)

### 2. intrusive_ptr 수명 관리

**결론: 양호**

- `Session::refcount_`는 non-atomic (per-core only) -- 적절함. cross-core에서 SessionId(enum class)만 전달
- `intrusive_ptr_release()` -> refcount 0일 때 pool_owner_ 체크 후 적절한 소멸 경로
- `Session::enqueue_write()` -> `SessionPtr self(this)` 캡처 -> write_pump 코루틴이 자기 Session을 생존 보장 -- 올바름
- `ConnectionHandler::read_loop()` -> SessionPtr 값 캡처 -> 코루틴 전체 생존 보장 -- 올바름

### 3. Strand 안전성 / 비동기 핸들러 executor

**결론: 양호**

- io_context-per-core 아키텍처에서 `concurrency_hint=1`이므로 implicit strand
- `Listener::on_accept()` -> `boost::asio::post(core_io, ...)` -- 올바르게 target core로 dispatch
- `ResponseDispatcher::on_response()` -> `boost::asio::post(engine_.io_context(target_core), ...)` -- 올바름
- `BroadcastFanout::fanout()` -> `cross_core_post(engine_, core_id, ...)` -- 올바름
- `Session::write_pump()` -> `socket_.get_executor()` 사용 -- 올바름

### 4. 데이터 레이스 / Lock-free 구조

**결론: 양호**

MpscQueue memory ordering 분석:
- `enqueue()`: head_ CAS `acq_rel`, tail_ load `acquire`, slot.ready store `release` -- 올바름
- `dequeue()`: tail_ load `relaxed` (consumer only), slot.ready load `acquire`, slot.ready store `release`, tail_ store `release` -- 올바름
- Producer가 data를 쓴 후 ready=true(release), consumer가 ready(acquire)를 본 후 data 읽기 -- happens-before 보장

SharedPayload refcount:
- `release()`: `fetch_sub(1, acq_rel)` -> prev == 1이면 delete -- 올바름 (acq_rel은 이전 코어의 데이터 쓰기가 보이도록 보장)

CoreEngine drain coalescing:
- `drain_pending_[target_core].exchange(true, acq_rel)` -> post handler -> store(false, release) + re-check -- 올바름

### 5. 코루틴 수명 / co_await 이후 참조 유효성

**결론: 양호 (서비스 코드 패턴 확인)**

- AuthService: `req->email()` 등을 co_await 전에 `std::string`으로 복사 -- 올바름
- ChatService: 동일 패턴 -- 올바름
- GatewayService: `handle_request()`에서 payload를 co_await 이전에 완전히 소비 -- 올바름
- `cross_core_call`: `@pre func MUST NOT capture coroutine-local variables by reference` 문서화됨, 실제 사용처도 준수

### 6. 커스텀 할당기

**결론: 양호**

SlabAllocator:
- slot_size 최소 sizeof(FreeNode) 보장, alignof(max_align_t) 정렬 -- 올바름
- double-free 감지: magic marker 기반 (best-effort, 문서화됨) -- 적절
- overflow check: `slot_size_ * count > SIZE_MAX` -- 올바름
- MSVC `_aligned_malloc`/`_aligned_free` 분기 -- 올바름

ArenaAllocator:
- alignment 계산: `(addr + align - 1) & ~(align - 1)` -- 올바름
- `align == 0` 또는 non-power-of-2 검사 -- 올바름
- reset() -> 첫 블록만 유지, 나머지 free -- 올바름
- move constructor/assignment -> `other.total_allocated_ = 0` 리셋 -- 올바름

BumpAllocator:
- zero capacity 허용 (경고 후 모든 allocate nullptr) -- 적절
- alignment 계산 올바름, overflow 체크(`result + size > end_`) -- 올바름

RingBuffer:
- power-of-2 capacity + mask 기반 wrap-around -- 올바름
- linearize() -> realloc 실패 시 기존 버퍼 보존 -- 올바름
- shrink_to_fit() hysteresis (4x -> 2x) -- 적절

### 7. Cross-core 통신

**결론: 양호**

- MpscQueue: ABA 문제 없음 (bounded queue with monotonic head/tail, slot reuse는 full revolution 후에만 가능)
- cross_core_call: CAS(0->1 completion, 0->2 timeout) race resolution -- 올바름
- broadcast_cross_core: 실패 시 `payload->release()` -- refcount leak 방지 올바름
- drain_remaining: stop()+join() 후 호출, LegacyCrossCoreFn task delete -- 올바름

### 8. TimingWheel

**결론: 양호**

- tick overflow: `current_tick_`는 uint64_t -- 584 billion years at 1 tick/ms -- 실질적으로 overflow 불가
- reschedule: remove_entry + insert_entry (same Entry pointer reuse) -- 올바름
- tick() 3-phase 설계: collect -> remove -> callback -- UAF 방지 올바름
- Phase 3a에서 entries_[id] = nullptr + active_count_-- (콜백 전) -- 콜백 내 cancel() 재진입 시 UAF 방지
- free_ids_.push_back 콜백 이후 -- 콜백 내 schedule() 시 ID 충돌 방지

---

## 발견 이슈

### [MAJOR] Session::write_pump pump_running_ 플래그 경합 가능성

- **파일**: `apex_core/src/session.cpp:118-125`, `apex_core/src/session.cpp:136-154`
- **내용**: `enqueue_write()`에서 `pump_running_`이 false일 때 true로 설정하고 `co_spawn`으로 write_pump를 기동한다. write_pump 종료 시 154번째 줄에서 `pump_running_ = false`로 리셋한다. 문제는 이 사이에 타이밍 윈도우가 존재한다는 것이다:
  1. write_pump이 `write_queue_.empty()`를 확인하여 true (queue 비어있음)
  2. 동시에 다른 코루틴이 `enqueue_write()`를 호출하여 write_queue_에 push
  3. `pump_running_`이 아직 true이므로 새 pump을 기동하지 않음
  4. write_pump이 while 루프를 빠져나와 `pump_running_ = false` 설정
  5. 큐에 데이터가 남아있지만 pump이 없는 상태 발생

  이 시나리오가 발생하려면 write_pump의 while 조건 검사와 pump_running_ 리셋 사이에 enqueue_write가 호출되어야 한다. 단일 io_context(implicit strand)에서는 co_await 지점에서만 다른 코루틴이 실행되므로, write_pump의 `while (!write_queue_.empty() && is_open())` -> `pump_running_ = false` 경로에서 co_await가 없어 원자적으로 실행된다.

  **그러나**, 이것은 현재 구현의 암묵적 보장에 의존하는 것이다. write_pump의 while 루프 탈출 조건이 `write_queue_.empty()`이고, 큐가 비었다는 것은 마지막 async_write의 co_await가 이미 완료되어 pop이 끝난 상태이다. 이 시점에서 다른 코루틴이 co_await 없이 while 조건 검사 -> `pump_running_ = false` 사이에 끼어들 수 없다.

  **결론**: 현재 구현은 implicit strand 보장 하에서 안전하다. 그러나 이 안전성 근거가 코드에 문서화되어 있지 않다. 향후 유지보수 시 while 루프 내에 co_await를 추가하면 깨질 수 있는 불변식이다.
- **조치**: 코드 주석으로 안전성 근거 명시 (직접 수정)

### [MINOR] ConsumerPayloadPool의 release()에서 lock 밖 delete 가능

- **파일**: `apex_shared/lib/adapters/kafka/src/consumer_payload_pool.cpp:74-97`
- **내용**: `release()` 함수에서 `buf->data.clear()`를 mutex 밖에서 수행하고, mutex 안에서 풀 반환 여부를 결정한다. max 초과 시 `delete buf`를 mutex 안에서 호출하는데, delete 자체는 allocator 호출이므로 lock 구간을 늘릴 수 있다. 하지만 이는 성능 최적화 관점이지 정확성 문제는 아니다. `data.clear()`가 lock 밖에서 호출되는 것도 해당 버퍼를 이 시점에 독점적으로 소유하므로 안전하다.
- **조치**: 보고만 (정확성 이슈 아님, BACKLOG 등록 불필요)

### [MINOR] RingBuffer의 non-power-of-2 capacity alloc_size 보정

- **파일**: `apex_core/src/ring_buffer.cpp:27`
- **내용**: `const size_t alloc_size = std::max(capacity_, size_t{64});`에서 capacity_가 이미 next_power_of_2로 올림되어 최소 1이다. 이 max()는 capacity < 64인 경우(1, 2, 4, 8, 16, 32)에 `_aligned_malloc`의 "size must be multiple of alignment" 요구사항을 충족시킨다. 올바르게 처리되고 있다. 다만, POSIX `std::aligned_alloc`의 Linux 경로에서는 `alloc_size`에 대한 별도 보정이 없어 capacity_ < 64일 때 "size가 alignment의 배수" 조건을 위반할 수 있다. 하지만 capacity_는 power-of-2이고 64 이하일 때 max(capacity_, 64)로 64가 선택되므로 항상 64의 배수이다. 문제없음.
- **조치**: 보고만 (이슈 아님)

### [MINOR] TSAN suppressions이 너무 광범위함

- **파일**: `tsan_suppressions.txt` (루트 + apex_core)
- **내용**: `race:boost::asio::detail::*`와 `race:boost::asio::io_context::*`가 Boost.Asio의 모든 내부 레이스를 억제한다. 이는 false positive를 제거하지만, 실제 Asio 사용 패턴에서 발생하는 진짜 레이스도 가릴 수 있다. 더 구체적인 함수 이름으로 범위를 좁히는 것이 바람직하다 (예: `race:boost::asio::detail::scheduler::do_run_one`, `race:boost::asio::detail::epoll_reactor::*`).
- **조치**: BACKLOG 등록 (MINOR, 개선 사항)

### [MINOR] Session::max_queue_depth_ 하드코딩

- **파일**: `apex_core/include/apex/core/session.hpp:179`
- **내용**: `size_t max_queue_depth_{256};`이 하드코딩되어 있다. ServerConfig에서 설정 가능하게 하는 것이 바람직하다.
- **조치**: 이미 BACKLOG #97에 등록됨 (중복 보고)

### [MINOR] ArenaAllocator total_allocated_ 초기값 생성자 불일치

- **파일**: `apex_core/src/arena_allocator.cpp:15`
- **내용**: 생성자에서 `total_allocated_(block_size)`로 초기화한 후 `blocks_.push_back(make_block(block_size_))`를 호출한다. `make_block`이 throw하면 `total_allocated_`가 `block_size`이지만 `blocks_`는 비어있는 불일치 상태가 된다. 그러나 이 경우 `bad_alloc`이 throw되어 객체 생성 자체가 실패하므로 (생성자 예외) 이 불일치 상태를 관찰할 수 있는 코드는 없다. 문제없음.
- **조치**: 보고만 (이슈 아님)

---

## 직접 수정 사항

### 1. write_pump 안전성 주석 보강

`session.cpp`의 write_pump 관련 코드에 implicit strand 기반 안전성 근거를 주석으로 명시한다.
