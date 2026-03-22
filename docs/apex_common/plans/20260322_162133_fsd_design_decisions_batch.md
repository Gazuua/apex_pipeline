# FSD 설계 결정 일괄 — #133, #112, #21, #22, #20

**날짜**: 2026-03-22
**목적**: FSD 자동화에서 구현 불가로 분류된 항목들의 설계 결정 확정. 다음 FSD에서 구현 착수 가능하도록 설계 방향 고정.

---

## #133. TransportContext의 ssl::context* — apex_core에 OpenSSL 직접 의존

**등급**: MAJOR | **스코프**: core, shared | **타입**: design-debt

### 문제

`TransportContext`가 `boost::asio::ssl::context*`를 직접 보유하여 apex_core에 OpenSSL 의존 발생. Transport concept은 정의되어 있으나(`T::Config`, `T::Socket`, `make_socket`/`async_handshake` 등) 실제 호출하는 코드가 없음. Session은 `tcp::socket`으로 하드코딩.

타입 소거가 두 레벨에서 필요:
- **Context 레벨**: TransportContext에서 ssl::context* 제거
- **Socket 레벨**: Session이 tcp::socket 외에 ssl::stream<tcp::socket>도 수용

### 검토한 안

| 안 | 요약 | 장점 | 단점 |
|----|------|------|------|
| A | Transport concept associated type으로 타입 소거 (Session<T> 템플릿화) | zero-overhead, concept 기반 | Session 템플릿 전파 → SessionManager, ConnectionHandler 등 코어 전체 영향 |
| B | **Virtual socket wrapper (SocketBase)** | Session/SessionManager 비템플릿 유지, 기존 패턴 일관성 | I/O에 virtual call (~2ns), 다만 syscall 비용 대비 0.001% 미만 |
| C | std::variant<tcp::socket, ssl::stream> | 힙 할당 없음, 비템플릿 | 새 소켓 타입 추가 시 variant 확장, 코어에 SSL 타입 노출 |

**CRTP 검토**: Session을 비템플릿으로 유지하면서 CRTP는 적용 불가 — 소켓 타입이 런타임에 결정되므로 타입 소거가 필수. CRTP는 타입을 지우지 않고 노출하는 패턴이라 이 경우 트레이드오프가 맞지 않음.

### 결정: B안 — Virtual SocketBase

**핵심 설계:**
- `SocketBase` virtual interface: async_read, async_write, close, is_open 등
- `TcpSocket`, `TlsSocket` 구현체
- Session이 `unique_ptr<SocketBase>` 보유
- Listener<P, T>가 `T::make_socket()` → SocketBase wrap → Session에 전달
- ssl::context는 Listener<P, TlsTcpTransport>가 멤버로 소유 → TransportContext에서 완전 제거

**근거:**
1. 템플릿 전파 범위 최소화 — Session/SessionManager/ConnectionHandler 비템플릿 유지
2. 기존 패턴 일관성 — AdapterInterface/ListenerBase가 이미 "cold path virtual, hot path template" 전략
3. I/O virtual dispatch 비용은 커널 syscall 대비 무시 가능 (~2ns vs 수 μs)
4. TLS Listener 구현과 자연스럽게 연동

---

## #112. lock-free SessionMap (concurrent_flat_map) 아키텍처 벤치마크

**등급**: MAJOR | **스코프**: core, tools | **타입**: perf

### 문제

Per-core vs Shared 아키텍처 벤치마크에서 "io_context 내부 큐가 진짜 병목인지" 결정적 검증이 필요. 현재 Shared 변형은 64-shard mutex를 사용하여 lock 오버헤드가 결과에 혼입. lock-free map으로 lock 오버헤드를 제거해도 Shared가 여전히 느리면 io_context 분리가 유일한 해법임을 증명.

### 검토한 안

| 안 | 요약 | 장점 | 단점 |
|----|------|------|------|
| A | **기존 bench_architecture_comparison.cpp에 3번째 변형 추가** | 동일 조건 비교, 최소 변경 | visitor API 패턴 차이로 워크로드 코드 약간 수정 |
| B | 별도 bench_concurrent_map.cpp 분리 (read/write 비율별 시나리오) | 세밀한 분석 | 기존 벤치마크와 조건 불일치 위험, 핵심 질문에서 벗어남 |
| C | A+B 하이브리드 | 둘 다 | 벤치마크 2개 관리 부담 |

### 결정: A안 — 기존 벤치마크에 LockFree 변형 추가

**핵심 설계:**
- `BM_Shared_LockFree_Stateful` 추가: `boost::concurrent_flat_map<SessionId, SessionState>` 사용
- 동일 워크로드: session lookup + state mutation, 동일 스레드 수
- 3자 비교: Per-core vs Shared+sharded_mutex vs Shared+concurrent_flat_map
- Boost 1.84.0+ concurrent_flat_map 이미 vcpkg에 존재

**근거:**
1. 핵심 질문이 하나 — "lock 제거 후에도 Shared가 느린가?"
2. 동일 파일·동일 조건에서 비교해야 결과가 결정적
3. 결과 불명확 시 B안 확장은 이후에도 가능

---

## #21. Server multi-listener dispatcher sync_all_handlers

**등급**: MAJOR | **스코프**: core | **타입**: design-debt

### 문제

Phase 3.5에서 multi-listener 핸들러 동기화가 `sync_default_handler`만 복사하고 개별 msg_id 핸들러는 동기화하지 않음. 서비스가 primary listener의 dispatcher에만 등록하므로 보조 리스너는 핸들러가 비어있음.

### 검토한 안

| 안 | 요약 | 장점 | 단점 |
|----|------|------|------|
| A | **sync_all_handlers() 단일 메서드** | 최소 변경, 현재 구조 유지 | std::function 복사 (startup cold path) |
| B | 서비스가 모든 리스너에 직접 등록 | 동기화 단계 제거 | ServiceBase→Listener 커플링 증가 |
| C | A + 프로토콜별 핸들러 필터링 | 잘못된 매칭 방지 | msg_id가 프로토콜 무관 설계라 필터링 기준 없음 |

### 결정: A안 — sync_all_handlers 단일 메서드

**핵심 설계:**
- `ListenerBase::sync_all_handlers(uint32_t core_id, const MessageDispatcher& source)` 추가
- `MessageDispatcher::handlers() const` 접근자 추가 (핸들러 맵 read-only 접근)
- Phase 3.5에서 `sync_default_handler` → `sync_all_handlers` 교체
- 전체 핸들러 맵 복사 (default + msg_id별)

**근거:**
1. msg_id 기반 핸들러는 프로토콜 무관 — 동일 핸들러를 모든 리스너에 복사하는 게 맞음
2. std::function 복사는 startup cold path
3. "primary에 등록 → 보조에 전파" 패턴이 서비스 API를 깨뜨리지 않음

---

## #22. async_send_raw + write_pump 동시 write 위험

**등급**: MAJOR | **스코프**: core | **타입**: design-debt

### 문제

Session에 두 가지 write 경로 존재:
- `async_send` / `async_send_raw`: 소켓에 `async_write` 직접 호출
- `enqueue_write` / `enqueue_write_raw`: write_queue → write_pump 순차 처리

둘 다 같은 소켓에 async_write하므로 동시 호출 시 UB. 현재는 enqueue_write_raw만 사용해서 회피 중이나 API 자체가 위험한 상태.

### 검토한 안

| 안 | 요약 | 장점 | 단점 |
|----|------|------|------|
| A | async_send_raw 삭제 (API 제거) | UB 원천 차단 | co_await send 패턴 불가 (응답→close 흐름 깨짐) |
| B | **async_send_raw를 write_pump 경유 + completion promise** | API 호환, co_await 가능, UB 제거 | WriteRequest에 promise 필드 추가 |
| C | write_pump 배타 잠금 | 직접 write 저레이턴시 유지 | 구현 복잡, co_await 인터리빙 문제 |

### 결정: B안 — write_pump 경유 + completion promise

**핵심 설계:**
- `async_send_raw` 시그니처 유지: `awaitable<Result<void>>`
- 내부적으로 `enqueue_write` + `WriteRequest`에 optional completion promise 추가
- write_pump가 해당 항목 처리 후 promise 이행 → 호출자 co_await 해제
- `async_send`도 동일 패턴 적용 (WireHeader 직렬화 후 enqueue)
- Boost.Asio `experimental::promise` 또는 단순 coroutine_handle 기반

**근거:**
1. 프레임워크가 동기/비동기 write를 모두 안전하게 제공해야 — API 제거는 직무 유기
2. "응답 보내고 → 세션 close" 패턴이 실재 (Gateway 에러 응답)
3. 외부 API 변경 없이 내부만 수정
4. Promise 패턴 구현 비용 낮음

---

## #20. BumpAllocator / ArenaAllocator 벤치마크

**등급**: MINOR | **스코프**: core | **타입**: perf

### 문제

벤치마크 스켈레톤(`bench_allocators.cpp`)이 "단순 alloc 반복"만 측정. 실제 서버 워크로드 패턴(요청/트랜잭션 스코프)을 반영하지 않아 서버 기본값(Bump 64KB, Arena 4KB block)의 최적성을 검증할 수 없음.

### 검토한 안

| 안 | 요약 | 장점 | 단점 |
|----|------|------|------|
| A | 실서비스 시뮬레이션 시나리오 추가 | 실 워크로드 기반 수치 | 시나리오에 가정 필요 |
| B | 파라미터 스윕 중심 | 최적 파라미터 탐색 | 시나리오 없이 수치 나열 |

### 결정: A+B 혼합

**핵심 설계:**

1. **기존 micro 유지** — 단순 alloc 성능 baseline
2. **BM_BumpAllocator_RequestCycle 추가**: 할당 3~8회 (가변 크기 32~512B) → reset 반복. 서버 기본값(64KB)이 request scope에 충분한지 검증
3. **BM_ArenaAllocator_TransactionCycle 추가**: 블록 경계 넘는 가변 할당 패턴 → reset 반복. 4KB block이 최적인지 검증
4. **Capacity 파라미터 추가**: Bump {16KB, 64KB, 256KB}, Arena block {1KB, 4KB, 16KB}

MixedWorkload는 제외 — bump과 arena를 동시에 쓰는 핫패스가 현재 없음.

---

## #61. 로그 보존 정책 TOML 파라미터화

**WONTFIX** — 로그 영구 보존은 의도적 설계. `logging.cpp`, `config.cpp`에 주석 명시 완료.
