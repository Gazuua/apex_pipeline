# Whisper O(1) Core Routing 설계 (BACKLOG-149)

## 개요

Whisper unicast 전송 시 `core_id=0` 하드코딩으로 인해 Gateway ResponseDispatcher가 모든 코어를 순회(O(N_cores))하는 비효율 제거. Auth SessionStore에 core_id를 함께 저장하여 O(1) 직접 전달로 개선.

## 현재 흐름 (O(N_cores))

```
Chat on_whisper()
  → Redis GET session:user:{uid} → session_id만 반환
  → send_response_with_flags(core_id=0, corr_id=0, session_id)
  → Kafka → Gateway ResponseDispatcher
    → corr_id==0 감지
    → for(core 0..N) boost::asio::post → find_session(session_id)
    → 1개 코어만 히트, 나머지 N-1은 헛 순회
```

## 목표 흐름 (O(1))

```
Chat on_whisper()
  → Redis GET session:user:{uid} → "session_id:core_id" 반환
  → send_response_with_flags(core_id=실제값, corr_id=0, session_id)
  → Kafka → Gateway ResponseDispatcher
    → corr_id==0 감지
    → core_id 유효 → post(core_id) → find_session(session_id) → 즉시 히트
```

## 설계

### 1. Redis 저장 형식 변경 (Auth)

**현재**: `session:user:{user_id}` → `"{session_id}"`
**변경**: `session:user:{user_id}` → `"{session_id}:{core_id}"`

예시: `"98765432101234567890:3"` (session_id=98765432101234567890, core_id=3)

- 단일 GET으로 session_id + core_id 동시 취득 (추가 라운드트립 없음)
- delimiter `:` 기반 합성 — Redis에서 가장 일반적인 복합값 패턴

### 2. Auth SessionStore API 변경

#### session_store.hpp

```cpp
// 변경 전
boost::asio::awaitable<Result<void>>
set_user_session_id(uint64_t user_id, uint64_t session_id);

// 변경 후
boost::asio::awaitable<Result<void>>
set_user_session_id(uint64_t user_id, uint64_t session_id, uint16_t core_id);
```

#### session_store.cpp

```cpp
// 값 합성: "{session_id}:{core_id}"
auto value = std::format("{}:{}", session_id, core_id);
co_return co_await set_with_ttl(session_key, value);
```

#### auth_service.cpp on_login

```cpp
// 변경 전
co_await session_store_->set_user_session_id(user_id, meta.session_id);

// 변경 후
co_await session_store_->set_user_session_id(user_id, meta.session_id, meta.core_id);
```

### 3. Chat on_whisper 파싱 변경

```cpp
// 변경 전: session_id만 파싱
auto target_session_id = safe_parse_u64(session_result->str, "whisper.target_session_id");

// 변경 후: "session_id:core_id" 파싱
auto [target_session_id, target_core_id] = parse_session_core(session_result->str);

// send_response_with_flags에서 실제 core_id 사용
send_response_with_flags(msg_ids::WHISPER_MESSAGE,
    envelope::routing_flags::DIRECTION_RESPONSE |
        envelope::routing_flags::DELIVERY_UNICAST,
    0 /* corr_id=0 for push */,
    target_core_id,          // ← 실제 core_id
    target_session_id,
    {fbb_msg.GetBufferPointer(), fbb_msg.GetSize()},
    "");
```

#### 파싱 헬퍼

```cpp
// chat_service.cpp 로컬 헬퍼
struct SessionCore {
    uint64_t session_id;
    uint16_t core_id;
};

Result<SessionCore> parse_session_core(std::string_view str);
// "98765432101234567890:3" → {session_id=98765432101234567890, core_id=3}
// ':' 없으면 → core_id=0 폴백 (하위 호환)
```

### 4. Gateway ResponseDispatcher 단일 코어 라우팅

```cpp
// 변경 전 (response_dispatcher.cpp, corr_id==0 경로)
for (uint16_t core = 0; core < session_mgrs_.size(); ++core) {
    boost::asio::post(engine_.io_context(core), [...] {
        auto session = session_mgrs_[core]->find_session(target_session_id);
        ...
    });
}

// 변경 후
if (corr_id == 0) {
    if (meta.core_id < session_mgrs_.size()) {
        // O(1): 지정된 코어에만 post
        boost::asio::post(engine_.io_context(meta.core_id), [...] {
            auto session = session_mgrs_[meta.core_id]->find_session(target_session_id);
            if (!session || !session->is_open()) return;  // stale core_id → 조용히 드롭
            session->enqueue_write(std::move(wire_response));
        });
    } else {
        // 폴백: core_id 범위 초과 시 기존 전수 검색 (하위 호환)
        for (uint16_t core = 0; core < session_mgrs_.size(); ++core) { ... }
    }
}
```

### 5. 엣지 케이스

| 시나리오 | 동작 | 이유 |
|---------|------|------|
| stale core_id (세션 종료 후 재접속) | 단일 코어에서 find_session 실패 → 조용히 드롭 | 세션이 없으면 어차피 전달 불가. 폴백 전수 검색은 불필요한 오버헤드 |
| Redis에 `:`가 없는 레거시 값 | `parse_session_core`가 core_id=0 폴백 | 롤링 배포 시 구버전 Auth가 쓴 값과 호환 |
| core_id >= num_cores | 전수 검색 폴백 | 서버 스케일다운 시 발생 가능, 안전하게 처리 |

## 영향 범위

| 파일 | 변경 내용 |
|------|----------|
| `auth-svc/include/apex/auth_svc/session_store.hpp` | `set_user_session_id` 시그니처에 `core_id` 추가 |
| `auth-svc/src/session_store.cpp` | 값 합성 포맷 `"{sid}:{core_id}"` |
| `auth-svc/src/auth_service.cpp` | on_login에서 `meta.core_id` 전달 |
| `chat-svc/src/chat_service.cpp` | `parse_session_core` 헬퍼 + on_whisper 파싱/라우팅 변경 |
| `gateway/src/response_dispatcher.cpp` | corr_id==0 경로에 단일 코어 분기 추가 |
| `auth-svc/tests/unit/test_session_store.cpp` | 새 포맷 검증 테스트 |
| `chat-svc/tests/unit/test_chat_handlers.cpp` | parse_session_core + on_whisper 테스트 |
| `gateway/tests/test_response_dispatcher.cpp` (신규 가능) | 단일 코어 라우팅 + 폴백 테스트 |

## 비변경 사항

- `SessionId` 구조 변경 없음 (enum class uint64_t 유지)
- `MetadataPrefix` 구조 변경 없음 (기존 core_id 필드 활용)
- `corr_id==0` 의미 변경 없음 (여전히 server push 마커)
- Gateway의 request-response 경로 (`corr_id != 0`) 변경 없음
