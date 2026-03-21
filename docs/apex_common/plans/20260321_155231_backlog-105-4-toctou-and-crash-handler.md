# BACKLOG-105, BACKLOG-4 — Chat TOCTOU 레이스 수정 + Assertion 크래시 로깅

- **브랜치**: `bugfix/backlog-105-4-toctou-race-and-assertion-logging`
- **작성일**: 2026-03-21 15:52:31

---

## 1. 목적

두 가지 독립적 이슈를 하나의 PR로 처리한다.

| 항목 | 내용 |
|------|------|
| **BACKLOG-105** | `ChatService::on_join_room`에서 SCARD→SADD 사이 TOCTOU 레이스로 max_members 초과 가능 |
| **BACKLOG-4** | Assertion 실패 또는 크래시 시그널 발생 시 위치 정보 없이 프로세스 종료 |

---

## 2. BACKLOG-105 — Chat join_room TOCTOU 레이스 수정

### 2.1 현황

`chat_service.cpp` `on_join_room()` (line 241-308):
```
Step 2: SISMEMBER → 이미 방에 있는지 확인
Step 3: SCARD → 현재 인원 수 조회           ← TOCTOU 윈도우 시작
Step 4: SADD → 멤버 추가                    ← TOCTOU 윈도우 종료
Step 5: SCARD → 새 인원 수 조회
```

동시에 2명이 입장 시도하면 둘 다 Step 3에서 "인원 OK" 판정을 받고 Step 4에서 SADD 성공 → max_members 초과.

### 2.2 해법: Redis Lua 스크립트

**업계 표준 패턴**: Redis 싱글 스레드 특성을 활용한 Lua 스크립트로 check-then-act를 원자적 실행.

```lua
-- KEYS[1] = chat:room:{room_id}:members
-- ARGV[1] = user_id
-- ARGV[2] = max_members
if redis.call('SISMEMBER', KEYS[1], ARGV[1]) == 1 then
    return -1  -- ALREADY_IN_ROOM
end
if redis.call('SCARD', KEYS[1]) >= tonumber(ARGV[2]) then
    return 0   -- ROOM_FULL
end
redis.call('SADD', KEYS[1], ARGV[1])
return redis.call('SCARD', KEYS[1])  -- 성공: 새 멤버 수
```

**반환값 규약**:
- `-1`: 이미 방에 있음 (ALREADY_IN_ROOM)
- `0`: 방 가득 참 (ROOM_FULL)
- `> 0`: 성공 (현재 멤버 수)

### 2.3 호출 방식

기존 `RedisMultiplexer::command()` 메서드로 EVAL 직접 호출. 기존 rate limiter 패턴(`redis_rate_limiter.cpp`)을 따라 모든 인자를 `%s` 문자열로 전달:
```cpp
auto max_str = std::to_string(max_members);
command("EVAL %s 1 %s %s %s", lua_script, members_key, user_id_str, max_str)
```

**Lua 스크립트 임베딩**: `R"lua(...)lua"` raw string literal로 `static constexpr` 변수에 저장 (rate limiter 패턴 동일).

- 어댑터 인터페이스 변경 없음
- EVALSHA 최적화는 향후 Lua 사용 확대 시 별도 백로그

### 2.4 에러 처리

EVAL 호출 결과에 대해:
- `!result.has_value()` → Redis 연결/어댑터 에러 — 기존 패턴대로 warn 로깅 + 에러 응답
- `result->is_error()` → Lua 실행 에러 (문법 오류, NOSCRIPT 등) — warn 로깅 + 에러 응답
- 정상 반환 → `result->integer` 값으로 -1/0/>0 분기

### 2.5 변경 파일

| 파일 | 변경 |
|------|------|
| `apex_services/chat-svc/src/chat_service.cpp` | `on_join_room()` — 5단계 Redis 호출을 EVAL 1회로 교체 |

### 2.5 대안 검토 (기각)

| 방식 | 기각 사유 |
|------|-----------|
| MULTI/EXEC + WATCH | MULTI 내부 조건 분기 불가, high contention 시 재시도 폭주 |
| Redis Functions (FCALL) | Redis 7.0+ 필수, 별도 배포 파이프라인 필요, 현재 시기상조 |
| Redlock | 단일 노드 Lua로 충분, 불필요한 복잡성 |
| App-level 직렬화 | SPOF + 스케일링 복잡 |

---

## 3. BACKLOG-4 — Assertion 크래시 위치 로깅

### 3.1 현황

- `SIGINT`/`SIGTERM`만 Boost ASIO `signal_set`으로 처리 (graceful shutdown)
- `SIGSEGV`/`SIGABRT`/`SIGBUS`/`SIGFPE` 미처리 → 로깅 없이 즉사
- 표준 `assert()` 사용 — 실패 시 SIGABRT 발생하지만 위치 정보 미기록
- 크래시 시그널에서는 `shutdown_logging()`이 호출되지 않으므로 비동기 큐에 남은 로그 유실 가능 (정상 종료 시에는 애플리케이션 레벨에서 호출됨)

### 3.2 구성 요소

#### A. APEX_ASSERT 매크로 (`apex_core/include/apex/core/assert.hpp`)

```cpp
#define APEX_ASSERT(cond, msg) \
    do { \
        if (!(cond)) [[unlikely]] { \
            apex::core::detail::assert_fail(#cond, msg, __FILE__, __LINE__, __func__); \
        } \
    } while (false)
```

`assert_fail()` 동작:
1. spdlog `critical` 레벨로 파일/함수/라인/조건/메시지 기록
2. `spdlog::default_logger()->flush()` — 비동기 큐 drain
3. `std::abort()` — SIGABRT 발생 → 크래시 핸들러로 이어짐

**Release 빌드에서도 활성** — 표준 `assert`와 달리 `NDEBUG`로 제거되지 않음.

#### B. 크래시 시그널 핸들러 (`apex_core/include/apex/core/crash_handler.hpp` + `src/crash_handler.cpp`)

**API**:
```cpp
namespace apex::core {
    void install_crash_handlers();
    void uninstall_crash_handlers();
}
```

**등록 시그널**:
- Linux/macOS: `SIGABRT`, `SIGSEGV`, `SIGBUS`, `SIGFPE`
- Windows: `SIGABRT`, `SIGSEGV`, `SIGFPE` + `SetUnhandledExceptionFilter`

**핸들러 동작** (async-signal-safe 고려):
1. stderr 직접 출력 — 시그널 이름 + 최소 정보 (signal-safe). Windows: `_write(2, ...)` 또는 `WriteFile(GetStdHandle(STD_ERROR_HANDLE), ...)`
2. `spdlog::default_logger()->flush()` — best-effort flush. **주의**: spdlog 내부 mutex와 데드락 가능성 있음 (크래시 시점에 spdlog가 write 중이면). 크래시 직전 최선의 시도이며, 데드락 시에는 step 1의 stderr 출력이 최소 진단 정보를 보장
3. 기본 시그널 핸들러 복원 (`SIG_DFL`)
4. `raise(sig)` — 코어 덤프 생성 허용

**Windows 추가 처리**:
- `SetUnhandledExceptionFilter` — SEH 미처리 예외 포착
- `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTHOOK)` — abort 시 Windows 팝업 억제

#### C. Server 통합

`Server::run()` 시작부에서 `install_crash_handlers()` 호출.
`config_.handle_signals`와 독립 — 크래시 핸들러는 항상 활성.

### 3.3 변경 파일

| 파일 | 변경 |
|------|------|
| `apex_core/include/apex/core/assert.hpp` | **신규** — APEX_ASSERT 매크로 |
| `apex_core/include/apex/core/crash_handler.hpp` | **신규** — crash handler API |
| `apex_core/src/crash_handler.cpp` | **신규** — crash handler 구현 |
| `apex_core/src/server.cpp` | `run()`에서 `install_crash_handlers()` 호출 |
| `apex_core/include/apex/core/server.hpp` | `assert()` → `APEX_ASSERT` 교체 |
| `apex_core/src/CMakeLists.txt` | 소스 파일 추가 |

### 3.4 기존 assert 교체

`server.hpp` 내 모든 `assert()` 호출을 `APEX_ASSERT`로 교체 (adapter, bump, arena, core_id 등).
같은 파일 내에서 assert 스타일이 혼재되는 것을 방지하며, `assert.hpp` include가 이미 추가되므로 추가 비용 없음.
`server.hpp` 외의 다른 파일 assert 사용처는 별도 작업으로 점진 교체.

---

## 4. 테스트 전략

| 항목 | 테스트 방식 |
|------|------------|
| #105 Lua 스크립트 정확성 | Lua 반환값 분기 로직 — E2E 기존 테스트(`e2e_chat_test.cpp`)로 정상 입장 검증 |
| #105 동시성 보장 | Redis Lua 싱글스레드 특성에 의해 보장 — 별도 동시성 테스트 불요 |
| #4 APEX_ASSERT 포맷 | 단위 테스트 — `assert_fail` 내부 포맷팅 로직 검증 (abort 미호출 모드) |
| #4 crash handler 등록 | 빌드 + 서버 시작/종료 정상 동작으로 간접 검증 |

---

## 5. 스코프 외

- EVALSHA 캐싱 최적화 → Lua 사용 확대 시 별도 백로그
- `on_leave_room`의 SISMEMBER→SREM 패턴 → 삭제는 TOCTOU 위험 낮음 (중복 SREM은 무해)
- 전체 codebase assert → APEX_ASSERT 일괄 교체 → 별도 작업
- Backtrace 출력 (libunwind/boost.stacktrace) → 별도 백로그 후보
