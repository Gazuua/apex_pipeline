# Business Logic Review — 전체 코드베이스

- **리뷰어**: Claude (auto-review)
- **리뷰 일시**: 2026-03-20 17:53:13
- **대상**: apex_core, apex_services (gateway, auth-svc, chat-svc), apex_shared (rate_limit)
- **범위**: 알고리즘 정확성, 에러 처리 경로, 상태 전이, 엣지 케이스, 정수 안전성, 코루틴 에러 전파

## 통계

| 등급 | 발견 | 수정 | 보고만 |
|------|------|------|--------|
| CRITICAL | 1 | 1 | 0 |
| MAJOR | 4 | 2 | 2 |
| MINOR | 3 | 0 | 3 |
| **합계** | **8** | **3** | **5** |

---

## 발견 이슈

### [CRITICAL]

#### C-1. `connection_handler.hpp` — async_send_raw와 write_pump 동시 실행으로 UB

- **파일**: `apex_core/include/apex/core/connection_handler.hpp:185-186`
- **설명**: `process_frames()`에서 핸들러 디스패치 에러 시 `co_await session->async_send_raw(error_frame)`을 호출했다. 핸들러가 `enqueue_write()`를 통해 write_pump 코루틴을 이미 기동한 상태라면, write_pump의 `async_write`와 이 `async_send_raw`의 `async_write`가 동일 소켓에서 동시에 실행되어 **Undefined Behavior**가 발생한다. Session 헤더 주석에 "Only one async_send ... may be in-flight per Session at any time" 경고가 명시되어 있다.
- **영향**: 소켓에 concurrent write → 데이터 인터리빙 → 프레임 깨짐 → 클라이언트 연결 끊김 또는 데이터 손상. 핸들러가 enqueue_write를 사용하는 모든 서비스(Gateway, Auth response 등)에서 발생 가능.
- **조치**: `async_send_raw` → `enqueue_write_raw`로 교체하여 write_pump를 통한 순차 전송 보장.

### [MAJOR]

#### M-1. `message_router.cpp` — corr_counter_ 오버플로 시 core_id 침범

- **파일**: `apex_services/gateway/src/message_router.cpp:60`
- **설명**: `generate_corr_id()`에서 `(core_id_ << 48) | (++corr_counter_)`를 반환한다. `corr_counter_`가 2^48을 초과하면 상위 16비트(core_id 영역)를 침범하여 잘못된 core_id가 인코딩된다. ResponseDispatcher가 이 core_id로 target core를 결정하므로, 응답이 **엉뚱한 코어의 PendingRequestsMap으로 전달**된다.
- **영향**: 응답 미전달(pending timeout) 또는 다른 세션에 오배달. 1M req/s 기준 약 8.9년 뒤 발생하여 실용적 문제는 낮으나, 장기 운영 서버에서 silent data corruption 가능.
- **조치**: `corr_counter_ & ((1ULL << 48) - 1)` 마스킹 적용. 카운터가 48비트를 초과하지 않도록 방어.

#### M-2. `gateway_service.cpp` — dynamic_cast null 체크 누락 (잠재적 nullptr 역참조)

- **파일**: `apex_services/gateway/src/gateway_service.cpp:257-258`
- **설명**: `create_globals()`에서 `dynamic_cast<GatewayService*>(state.services[0].get())`의 반환값을 null 체크 없이 바로 역참조했다. `services[0]`이 GatewayService가 아닌 다른 서비스이면 nullptr → **크래시**.
- **영향**: 서비스 등록 순서가 변경되면 서버 시작 시 크래시. 현재 코드에서는 항상 GatewayService가 먼저 등록되지만, 리팩터링 시 깨질 수 있는 취약한 가정.
- **조치**: null 체크 + `std::logic_error` throw 추가. 디버그 빌드에서 조기 감지.

#### M-3. `auth_service.cpp` — locked_until 만료 시각 미검증 (보고만)

- **파일**: `apex_services/auth-svc/src/auth_service.cpp:162`
- **설명**: `is_locked = !pg_res.is_null(0, 2)` — `locked_until` 컬럼이 NOT NULL이면 무조건 잠금 처리한다. `locked_until`이 과거 시각이어도 잠금으로 판정되어 로그인이 영구 차단된다.
- **영향**: 일시 정지(temporary ban) 기능이 의도대로 동작하지 않을 수 있음. 영구 잠금만 사용한다면 문제없으나, 향후 일시 정지 도입 시 버그화.
- **조치**: SQL 쿼리에 `AND locked_until > NOW()` 조건 추가 검토 필요. 설계 의도 확인 후 수정. (보고만 — Auth 서비스 설계 결정 사항)

#### M-4. `chat_service.cpp` — join_room SCARD/SADD 사이 TOCTOU (보고만)

- **파일**: `apex_services/chat-svc/src/chat_service.cpp:282-294`
- **설명**: `on_join_room()`에서 Redis SCARD로 멤버 수 확인 후 SADD로 추가한다. 두 명령 사이에 다른 요청이 SADD를 실행하면 `max_members` 제한을 초과할 수 있다. Redis 트랜잭션(MULTI/EXEC) 또는 Lua 스크립트로 원자적 처리가 필요하다.
- **영향**: 동시 가입 요청 시 방 인원 제한 초과 가능. 대규모 트래픽에서 race 확률 증가.
- **조치**: Redis Lua 스크립트로 SCARD 확인 + SADD를 원자적 실행하도록 변경 필요. (보고만 — Chat 서비스 스코프)

### [MINOR]

#### m-1. `sliding_window_counter.cpp` — window_size가 0이면 allow()에서 zero division

- **파일**: `apex_shared/lib/rate_limit/src/sliding_window_counter.cpp:19`
- **설명**: 생성자에서 `window_size.count() > 0 ? window_size : Duration{1}`로 0을 방어하지만, `Duration{1}`은 `steady_clock::duration`의 1 나노초이므로 사실상 모든 요청이 차단된다. 사용자가 0을 전달하면 "rate limit 비활성화"를 기대할 수 있으나 실제로는 극단적 제한이 적용된다.
- **영향**: 설정 실수 시 서비스 거부. 현재 코드에서 0이 전달되는 경로 없음.
- **조치**: 보고만 — 방어 코드 자체는 존재하며 실제 호출 경로에서 발생하지 않음.

#### m-2. `per_ip_rate_limiter.cpp` — remove_entry의 lru_index 유효성 검증 부족

- **파일**: `apex_shared/lib/rate_limit/src/per_ip_rate_limiter.cpp:144`
- **설명**: `remove_entry()`에서 `lru_idx < lru_order_.size()` 검증 후 swap + pop_back을 수행하지만, 이미 entries에서 삭제된 IP와 lru_order_의 일관성이 깨질 수 있는 경로가 이론적으로 존재한다 (TTL 콜백과 evict_lru의 순서 의존).
- **영향**: 실제 운영에서는 per-core 단일 스레드 보장으로 발생하지 않음.
- **조치**: 보고만 — 현재 아키텍처에서 안전.

#### m-3. `bump_allocator.cpp` — allocate() 포인터 산술 오버플로 이론적 가능

- **파일**: `apex_core/src/bump_allocator.cpp:72-73`
- **설명**: `result + size > end_` 비교에서 `result + size`가 이론적으로 오버플로할 수 있다. 실제로 BumpAllocator의 용량은 수 MB 이내이므로 64비트 주소 공간에서 오버플로 불가.
- **영향**: 없음.
- **조치**: 보고만 — 실용적 문제 없음.

---

## 검증 완료 항목 (이슈 없음)

### 알고리즘 정확성
- **MpscQueue**: CAS 루프 정확, tail reload로 spurious full 방지, 64-bit size_t static_assert로 인덱스 오버플로 방지
- **TimingWheel**: 4-phase tick (수집 → 제거 → entries 정리 → 콜백+해제)으로 콜백 내 cancel() 재진입 UAF 방지. free_ids 반환이 콜백 후에 수행되어 ID 충돌 없음
- **RingBuffer**: power-of-2 마스킹, linearize()의 defensive copy(hdr_buf), shrink_to_fit() hysteresis 모두 정확
- **SlabAllocator**: double-free 감지(MAGIC), auto-grow 오버플로 체크, 슬롯 정렬 검증 올바름
- **FrameCodec**: try_decode()에서 header를 로컬 복사 후 두 번째 linearize() 호출 — 기존 span 무효화 방어 정확

### 에러 처리 경로
- **Session**: close() 멱등성, 소멸자에서 close() 호출, write_pump 에러 시 큐 정리 + close()
- **cross_core_call**: CAS로 timeout/completion 레이스 해결, post 실패 시 task delete, 타이머 취소 post 안전
- **CoreEngine**: drain_inbox에서 task 예외 catch, drain_remaining으로 shutdown 시 메모리 누수 방지
- **AuthService**: 모든 PG/Redis 에러 경로에서 에러 응답 전송 또는 non-fatal 로깅. FlatBuffers 필드 null 체크 완비
- **ChatService**: 8개 kafka_route 핸들러 모두 에러 경로에서 클라이언트에 에러 응답 전송

### 상태 전이
- **Session 생명주기**: Connected → Active → Closed. set_state()에 Closed 전이 assert. close() 멱등
- **Server 5-phase 라이프사이클**: configure → wire → start → handler sync → adapter wiring 순서 올바름
- **GatewayPipeline**: 3-layer 파이프라인 (IP → JWT → user → endpoint) 순서 정확, fail-open 전략 일관

### 엣지 케이스
- **빈 payload**: 모든 FlatBuffers 핸들러에서 null 체크 수행
- **소켓 EOF**: connection_handler에서 break → remove_session → close() 순서 정확
- **max_entries 초과**: PerIpRateLimiter의 LRU eviction, PendingRequestsMap의 max_entries 체크 모두 동작
- **zero-capacity 할당기**: BumpAllocator(0)은 warn 로그 + 모든 allocate에서 nullptr 반환

### 정수 안전성
- **MpscQueue**: 64-bit size_t static_assert, power-of-2 마스킹으로 인덱스 안전
- **WireHeader**: `body_size` uint32_t + MAX_BODY_SIZE 16MB 검증, `frame_size()` 반환 size_t (64-bit static_assert)
- **SlabAllocator**: `slot_size_ * count` 오버플로 체크 (`SIZE_MAX / count`)

### 코루틴 에러 전파
- **모든 co_await 경로**: AuthService, ChatService, GatewayPipeline 모두 co_await 반환값을 검사하고 에러 전파 또는 에러 응답 전송
- **co_await 전 FlatBuffers 포인터 복사**: AuthService/ChatService에서 `req->email()->string_view()` 등을 co_await 전에 `std::string`으로 복사 — FlatBuffers 포인터 무효화 방어 완비
- **as_tuple 패턴**: Session의 async_write에서 `as_tuple(use_awaitable)`로 예외 대신 error_code 반환 — 일관됨
