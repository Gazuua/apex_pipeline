# Acceptor Per-IP 연결 제한 설계 (BACKLOG-256)

## 개요

Listener/Acceptor 프레임워크 레벨에서 per-IP 연결 수를 제한하여 connection exhaustion 공격을 방어한다. Gateway의 per-IP rate limiting(요청 수/초)과 상호보완 — rate limit은 "요청 폭주"를 막고, 이것은 "연결만 열고 가만히 있는" 공격을 막는다.

## 아키텍처 결정: Owner-Shard 패턴

Seastar 프레임워크의 표준 패턴을 채택한다. `hash(IP) % num_cores`로 각 IP의 담당 코어를 결정하고, 해당 코어만 그 IP의 연결 카운터를 보유한다.

**선택 근거**:
- shared-nothing per-core 원칙 **완전 준수** — mutex, atomic, CAS 일체 사용 안 함
- 각 IP의 카운터는 단일 코어의 io_context에서만 접근 → 진정한 no-locking
- 기존 `cross_core_call` / `cross_core_post` SPSC 인프라 활용 (~0.2-1μs)
- accept는 cold path (초당 수백~수천) → cross-core 1회 오버헤드 무시 가능
- 모든 플랫폼/모드 호환 (Windows 단일 acceptor, Linux reuseport 모두)

**비교 검토한 대안**:
- Nginx 방식 (shared memory + mutex): shared-nothing 위반
- HAProxy 방식 (shared atomic map): shared-nothing 위반
- eBPF IP-affinity steering: 구현 비용 높고 로드 밸런싱 불균형 → BACKLOG-259로 등록

## 설계

### 1. `ConnectionLimiter` 클래스

```
apex_core/include/apex/core/connection_limiter.hpp
apex_core/src/connection_limiter.cpp
```

per-core 인스턴스. 각 인스턴스는 `hash(IP) % num_cores == my_core_id`인 IP들의 카운터만 보유.

```cpp
class ConnectionLimiter
{
public:
    ConnectionLimiter(uint32_t core_id, uint32_t num_cores, uint32_t max_per_ip);

    // 로컬 전용 (담당 코어에서만 호출)
    bool try_increment(std::string_view ip);  // count < max면 증가 후 true
    void decrement(std::string_view ip);      // count 감소, 0이면 엔트리 제거

    // 담당 코어 결정
    static uint32_t owner_core(std::string_view ip, uint32_t num_cores);

private:
    uint32_t core_id_;
    uint32_t num_cores_;
    uint32_t max_per_ip_;
    boost::unordered_flat_map<std::string, uint32_t> counts_;
    ScopedLogger logger_;
};
```

`owner_core(ip, num_cores)` = FNV-1a 또는 std::hash 기반. 결정론적, 빠르면 됨.

### 2. Cross-Core 호출 흐름

**Accept 시** (Listener `on_accept` 콜백):

```
acceptor core (any)
  → remote_endpoint로 IP 추출
  → owner = ConnectionLimiter::owner_core(ip, num_cores)
  → if (owner == current_core)
      → limiter.try_increment(ip) 로컬 호출
    else
      → co_await cross_core_call(owner, [&] { return limiter.try_increment(ip); })
  → 거부 시 socket.close() + 로그
  → 허용 시 기존 흐름
```

**Session Close 시** (SessionManager remove_callback):

```
session's core (any)
  → session->remote_ip() 로 IP 획득
  → owner = ConnectionLimiter::owner_core(ip, num_cores)
  → if (owner == current_core)
      → limiter.decrement(ip) 로컬 호출
    else
      → cross_core_post(owner, [&] { limiter.decrement(ip); })
        (fire-and-forget — release는 실패해도 치명적이지 않음)
```

### 3. Session에 IP 캐시

Session close 시 소켓이 이미 닫혀 remote_endpoint()가 실패할 수 있으므로, accept 시점에 IP를 캐시한다.

```cpp
// session.hpp에 추가
class Session {
    std::string remote_ip_;  // accept 시 설정, 이후 read-only
public:
    void set_remote_ip(std::string ip);
    const std::string& remote_ip() const noexcept;
};
```

설정 시점: `ConnectionHandler::accept_connection()`에서 `session->set_remote_ip(ip)`.

### 4. ServerConfig 확장

```cpp
// server_config.hpp에 추가
uint32_t max_connections_per_ip = 100;  // 0 = 비활성
```

`max_connections_per_ip == 0`이면 ConnectionLimiter를 생성하지 않아 기존 동작과 동일.

### 5. Server 통합

```cpp
// server.hpp
class Server {
    // per-core ConnectionLimiter
    std::vector<std::unique_ptr<ConnectionLimiter>> per_core_limiters_;
};
```

`Server::run()` 초기화 시 `config_.max_connections_per_ip > 0`이면 per-core 인스턴스 생성.
Listener에 limiter 포인터 벡터 전달.

### 6. Graceful Shutdown 순서

현재 `finalize_shutdown()` 순서:

```
stop() → listener.drain() (accept 중단)
begin_shutdown() → session 전체 close → poll (drain 대기)
finalize_shutdown():
  0. HTTP 서버 정지
  1. Listener 완전 정지
  2. Adapter drain
  3. Scheduler 정지
  4. Service 정지
  4.5. Outstanding 코루틴 drain
  5. Adapter close
  6. CoreEngine stop + join + drain_remaining()  ← 잔여 SPSC 메시지 처리
  6.5. BlockingTaskExecutor shutdown
  7. globals_ clear
  ~Server(): 멤버 역순 소멸
```

**ConnectionLimiter 안전 보장**:

1. `listener.drain()` 이후 새 `try_accept` 호출 없음 → increment 중단
2. `poll_shutdown()` 중 세션 close → `release`(decrement) cross_core_post 발행
3. Step 6의 `drain_remaining()`이 잔여 SPSC 메시지를 모두 소진 → release 유실 없음
4. `per_core_limiters_`는 Server 멤버 → `~Server()`에서 Step 7 이후 소멸 → CoreEngine 정지 후라 안전

**핵심 불변식**: ConnectionLimiter는 CoreEngine보다 늦게 소멸해야 한다.

**이중 보호 전략**:
1. **명시적 소멸 (primary)**: `finalize_shutdown()` Step 6 (CoreEngine stop+join+drain_remaining) 직후에 `per_core_limiters_.clear()`를 명시적 호출. 기존 패턴과 일관적 (globals_.clear() 등).
2. **RAII 역순 소멸 (backup)**: `per_core_limiters_`를 `core_engine_`보다 앞에 선언하여, finalize_shutdown 미호출 비정상 경로에서도 역순 소멸로 안전. 코드에 주석으로 의도 표현.

RAII에만 의존하지 않는다 — shutdown 순서는 코드로 강제하고, RAII는 비정상 경로의 안전망.

### 7. Listener 통합 상세

Listener는 템플릿 클래스 (`Listener<Protocol, Transport>`)이므로 ConnectionLimiter 포인터를 주입받는다:

```cpp
// listener.hpp — 생성자 또는 setter
void set_connection_limiters(std::vector<ConnectionLimiter*> limiters);
```

accept 콜백 내부에서:
1. 기존 `max_connections` (전역 한도) 체크 — 그대로 유지
2. **추가**: `max_connections_per_ip` 체크 — ConnectionLimiter cross_core_call

주의: 현재 `on_accept` 콜백은 동기인데, `cross_core_call`은 awaitable이다. reuseport 경로는 이미 코루틴 내부라 `co_await` 가능하지만, 단일 acceptor 경로의 `on_accept_` 콜백 시그니처가 `void(socket)`이므로 **코루틴으로 래핑**해야 한다. 이 부분은 구현 시 `on_accept` 콜백을 `co_spawn`으로 감싸는 방식으로 해결.

### 8. 엣지 케이스

| 시나리오 | 동작 |
|---------|------|
| `remote_endpoint()` 실패 | IP = "unknown", limiter 체크 스킵 (연결 허용) |
| 같은 IP가 빠르게 connect/disconnect 반복 | cross_core_call로 정확한 카운트 유지 |
| CoreEngine 미시작 상태에서 accept | limiter nullptr → 체크 스킵 |
| `max_connections_per_ip = 0` | limiter 미생성, 기존 동작 |
| num_cores = 1 | owner_core 항상 0, cross_core 불필요 (로컬 호출만) |

## 영향 범위

| 파일 | 변경 |
|------|------|
| `connection_limiter.hpp/cpp` (신규) | owner-shard per-IP 카운터 |
| `server_config.hpp` | `max_connections_per_ip` 추가 |
| `session.hpp/cpp` | `remote_ip_` 필드 + 접근자 |
| `listener.hpp` | limiter 주입 + accept 체크 |
| `connection_handler.hpp` | Session에 IP 설정 |
| `server.hpp/cpp` | ConnectionLimiter 생성 + Listener 주입 + 멤버 선언 순서 |
| `config.cpp` | TOML 파싱에 `max_connections_per_ip` 추가 |
| 테스트 (신규) | ConnectionLimiter 단위 테스트 |

## 비변경 사항

- Gateway rate limiting 코드 변경 없음 (독립 레이어)
- SessionManager API 변경 없음 (remove_callback 활용)
- MetadataPrefix 변경 없음
- 기존 `max_connections` (전역 한도) 로직 유지
