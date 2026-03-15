# v0.5 Wave 2 설계 — 서비스 체인 풀 패키지

> **작성일**: 2026-03-16
> **버전**: v0.5.0.0 → v0.5.4.0
> **상태**: Draft
> **선행**: Wave 1 완료 (Protocol concept, 어댑터 회복력, per-session write queue, WebSocket MVP)

---

## 1. 개요

### 1.1 목표

Wave 2는 Apex Pipeline의 **서비스 체인을 실제로 구동**하는 단계다. Gateway MVP를 구축하고, Auth Service로 인증 파이프라인을 완성하며, Chat Service로 실전적 Logic Service를 검증한 뒤, E2E 통합 테스트로 전체 경로를 증명한다.

### 1.2 범위 (A안: 풀 패키지)

| 마일스톤 | 버전 | 내용 |
|----------|------|------|
| Gateway MVP | v0.5.2.0 | TLS, JWT, msg_id 라우팅, 와이어 프로토콜 v2, Kafka Envelope |
| Rate Limiting | v0.5.2.1 | 3계층 Rate Limiter (Per-IP / Per-User / Per-Endpoint) |
| Auth Service | v0.5.3.0 | Kafka 기반 요청-응답 인증, JWT 발급/갱신, 세션 관리 |
| Chat Service | v0.5.3.1 | 채팅 로직 서비스 (방 관리, 메시지, 브로드캐스트, 히스토리) |
| E2E 통합 | v0.5.4.0 | Client → Gateway → Kafka → Service → 응답/브로드캐스트 전체 경로 |

### 1.3 Chat Service를 Logic Service로 채택한 이유

- 파이프라인의 **모든 메시지 경로**(요청-응답, 브로드캐스트, 히스토리)를 한 서비스에서 검증 가능
- 게임 서버에서 채팅은 가장 보편적인 실시간 서비스 — 실전적이면서도 도메인 복잡도가 적절
- Redis Pub/Sub, Kafka, PostgreSQL을 모두 사용하므로 어댑터 회복력(Wave 1)도 자연스럽게 검증

---

## 2. 와이어 프로토콜

### 2.1 WireHeader v2 (클라이언트 ↔ Gateway)

```
ver(u8) | flags(u8) | msg_id(u32) | body_size(u32) | reserved(u16) = 12바이트 (총 12바이트 정렬 + 향후 확장 예약)
```

**핵심 변경: msg_id를 u16 → u32로 확장.**

지금이 와이어 프로토콜을 변경할 수 있는 마지막 기회다. u16(65,536개)은 서비스 수와 메시지 종류가 늘어나면 부족해지고, retired msg_id 재사용을 영원히 금지하는 정책과도 맞지 않는다. u32(4.2B)면 실질적으로 고갈 걱정이 없다.

**마이그레이션 정책: Breaking Change 허용** (외부 클라이언트 미존재)

**수정 대상 체크리스트:**

- `wire_header.hpp`: msg_id u16→u32, 헤더 크기 10→12, CURRENT_VERSION 1→2
- `frame_codec.hpp/cpp`: encode/decode 로직
- `MessageDispatcher`: 키 타입 u16→u32, `flat_map<uint32_t, Handler>`
- `ServiceBase::route<T>()`: msg_id 파라미터 타입 변경
- 모든 Protocol 구현체: TcpBinaryProtocol, WebSocketProtocol의 try_decode/encode
- `connection_handler.hpp`: `dispatcher_.dispatch(session, header.msg_id, ...)` — msg_id 타입 암시적 변환 점검
- `error_sender.hpp/cpp`: `build_error_frame(header.msg_id, ...)` — msg_id 파라미터 타입
- `session.hpp`: `async_send(const WireHeader&, ...)` — WireHeader 구조체 직접 사용
- 테스트 전체: WireHeader를 생성하는 모든 테스트 파일
- msg_registry.toml: ID를 u32 범위로 기재

**전환 순서:**

1. WireHeader 구조체 + FrameCodec 수정
2. MessageDispatcher + ServiceBase 타입 변경
3. Protocol 구현체 업데이트
4. 테스트 일괄 수정
5. 빌드 + 전체 테스트 통과 확인

### 2.2 Kafka Envelope (Gateway ↔ Services)

내부 통신용 봉투는 **진화 가능성**을 핵심 설계 원칙으로 삼는다. Routing Header는 불변, Metadata는 버전별 진화.

**Routing Header (8바이트, 불변):**

```
header_version(u16) | flags(u16) | msg_id(u32)
```

**Metadata Prefix (32바이트, 고정):**

```
meta_version    u32      4
core_id         u16      2    ← 응답 라우팅 시 원래 core 식별
corr_id         u64      8
source_id       u16      2    ← 발신 서비스 식별 (0=시스템/Gateway, 1=Auth, 2=Chat, ...). 용도: 로깅, 디버깅, 메시지 출처 추적. 내부 프로토콜이라 meta_version 올려서 변경 가능
session_id      u64      8
timestamp       u64      8
                         ──  32 bytes
```

- channel_len, channel(var) 제거 — 브로드캐스트 시 채널명은 **FlatBuffers payload에 포함**
- flags의 전달 방식 비트(bit 2-3)로 브로드캐스트 여부 판별, 채널명은 payload 파싱으로 획득
- metadata가 고정 크기 → payload 시작 오프셋이 항상 Routing Header(8B) + Metadata(32B) = **40바이트**

**FlatBuffers Payload:** msg_id에 따라 스키마 결정.

### 2.3 Routing Header flags (u16)

| Bit | 용도 |
|-----|------|
| 0 | 방향 (0=요청, 1=응답) |
| 1 | 에러 |
| 2-3 | 전달 방식 (00=단일, 01=채널, 10=전역, 11=예약) |
| 4 | 압축 |
| 5 | 암호화 |
| 6-7 | 우선순위 (00=일반, 01=높음, 10=긴급, 11=시스템) |
| 8-9 | TTL 힌트 (00=기본, 01=짧음, 10=긺, 11=영구) |
| 10 | 프래그먼트 (대용량 분할) |
| 11-15 | 예약 |

flags를 비트맵으로 설계한 이유: 별도 필드를 추가하지 않고도 메시지 특성을 표현할 수 있어 헤더 크기를 8바이트로 고정할 수 있다. 예약 비트 6개로 향후 확장 여지도 충분.

### 2.4 Gateway 변환 흐름

- **요청**: WireHeader.msg_id(u32) → Routing Header.msg_id(u32) **그대로 복사**. Gateway가 corr_id, core_id, source_id, timestamp를 생성하여 Metadata에 추가.
- **응답**: Routing Header → WireHeader 역변환. Metadata는 Gateway가 소비(core_id로 대상 코어 식별 → corr_id로 세션 매칭)하고 클라이언트에 전달하지 않음.

---

## 3. Gateway 설계 (v0.5.2.0)

### 3.1 핵심 원칙

> **Gateway는 개별 서비스의 도메인 지식에 절대 의존 금지.**

서비스 추가/변경 시 Gateway 코드가 바뀌면 MSA가 아니다. Gateway는 범용 인프라(session, channel, routing)만 보유하고, 서비스별 로직은 config(TOML)로 주입한다.

### 3.2 TLS

**결정: Gateway 내부에서 직접 처리** (Boost.Asio `ssl::context`).

외부 TLS termination(Envoy, nginx 등)은 추가 홉과 운영 복잡도를 유발한다. 프레임워크 자체가 TLS를 지원해야 게임 서버 배포 시나리오(베어메탈, 단일 바이너리)에서도 동작한다.

- **Transport concept**을 core에 정의, OpenSSL 기반 구현(`TlsTcpTransport`, `PlainTcpTransport`)은 shared에 위치
- `listen<Protocol, Transport>` — 템플릿 인자 2개 고정
- TLS는 Transport의 변종이지 Protocol의 변종이 아님 — 관심사 분리
- **per-core TLS 컨텍스트** (Seastar 방식): SSL_CTX를 core별로 격리하여 lock contention 제거

**Transport concept 최소 요구사항:**

다음은 Transport concept의 의사코드이며, 실제 구현 시 C++20 concept 문법에 맞게 재정의한다 (requires-expression의 파라미터 타입 명시 등).

```cpp
template<typename T>
concept Transport = requires {
    typename T::Config;
    typename T::Socket;
} && requires(T::Socket& sock, auto& buf) {
    { T::async_accept(acceptor, sock) } -> awaitable<Result<void>>;
    { T::async_read(sock, buf) } -> awaitable<Result<size_t>>;
    { T::async_write(sock, buf) } -> awaitable<Result<size_t>>;
    { T::async_shutdown(sock) } -> awaitable<void>;
};
```

- TlsTcpTransport는 추가로 `async_handshake(sock, ssl_ctx)` 제공
- PlainTcpTransport는 handshake가 no-op

**기존 코드 전환 계획:**

- `Server::listen<P>` → `Server::listen<P, T>` 메서드 시그니처에 Transport 템플릿 파라미터 추가
- `Listener<P>` → `Listener<P, T>` (server.hpp, listener 관련)
- `Listener<P>` 내부 `PerCoreHandler::handler` 타입이 `ConnectionHandler<P, T>`로 변경
- `ListenerBase` 인터페이스는 Transport 정보 불필요 (virtual base, 변경 없음)
- `ConnectionHandler` → Transport::Socket 타입 추상화
- `TcpAcceptor` → Transport::async_accept로 대체
- Session 내부 소켓 타입: `Transport::Socket`으로 템플릿화
- 영향 범위: server.hpp, session.hpp, connection_handler, tcp_acceptor + 관련 테스트 전체

### 3.3 JWT

**결정: jwt-cpp (header-only, OpenSSL 기반).**

- 순수 header-only라 빌드 의존성 최소
- OpenSSL은 TLS에서 이미 사용하므로 추가 의존 없음
- **블랙리스트 전략 (C안)**: 로컬 시그니처 검증(hot path) + 민감 작업만 Redis 블랙리스트 조회(cold path)
  - 로그아웃, 비밀번호 변경 등 보안 민감 작업만 Redis 왕복 → 평상시 네트워크 비용 제로
  - **민감 작업 판정**: TOML config에서 민감 msg_id 목록 지정 (Gateway 재시작 없이 hot-reload 가능)
  - Access Token이 짧은 수명(15~30분)이므로 전수 검사 불필요

### 3.4 메시지 라우팅

**결정: msg_id 범위 기반 라우팅 (u32).**

서비스별 msg_id를 열거하는 방식은 Gateway가 메시지 목록을 알아야 하므로 도메인 의존이 생긴다. 범위 기반이면 Gateway는 숫자 범위만 보고 라우팅하므로 서비스 독립성이 유지된다.

- **기본 컨벤션**: 1,000개 단위 블록 → 최대 약 65개 서비스 (u16 범위 기준, u32 확장 시 사실상 무제한)
- **TOML config**로 서비스별 자유 범위 지정 (비연속 범위도 지원)
- **범위 테이블 이진 탐색** (`std::upper_bound`): 서비스 수가 수십 개 수준이므로 O(log N)이면 충분. 해시맵은 범위 매칭에 부적합.
- **서비스 0 = 시스템/Gateway 예약** (msg_id 0~999), 실제 서비스는 1번부터

### 3.5 TOML Hot-Reload & Admin API

- **1단계(Wave 2)**: FileWatcher 기반 TOML hot-reload — 라우팅 테이블, Rate Limit 설정 등을 Gateway 재시작 없이 반영
  - 크로스 플랫폼: Windows(ReadDirectoryChangesW)와 Linux(inotify) 추상화 필요, Boost.Asio file watcher 또는 자체 구현
- **2단계(향후)**: Admin API (Gateway 내장 8081 포트) — REST로 설정 조회/변경
- **3단계(향후)**: Dashboard — 정적 HTML에서 Admin API 호출
- **Admin 접근 제어**: 내부망 격리 (bind 127.0.0.1 또는 내부 VLAN), 추후 API Key/mTLS 추가

### 3.6 Rate Limiting (v0.5.2.1)

**결정: 3계층 Sliding Window Counter.**

Token Bucket/Leaky Bucket 대비 Sliding Window Counter는 구현이 단순하면서도 시간 경계(boundary) 문제가 없다. 처음부터 Sliding Window로 가는 이유는 Fixed Window의 boundary burst를 나중에 패치하는 것보다 깔끔하기 때문.

| 계층 | 위치 | 저장소 | 파이프라인 위치 |
|------|------|--------|-----------------|
| Per-IP | TLS 직후, JWT 이전 | Gateway 로컬 메모리 | 인증 전 공격 차단 |
| Per-User | JWT 이후 | Redis Lua 스크립트 | 인증된 사용자 제한 |
| Per-Endpoint | 라우팅 직전 | Redis Lua 스크립트 | msg_id별 세밀한 제한 |

- Per-IP를 로컬 메모리에 둔 이유: TLS 핸드셰이크 직후, JWT 파싱 전에 동작해야 하므로 Redis 왕복을 피해야 함
- Per-Endpoint: 기본값 + 선택적 오버라이드 패턴 (특정 msg_id에만 다른 한도 적용)

**Per-IP: per-core 구조 (lock 제로)**

- 총 한도 / 코어 수 = per-core 한도 (LINE wall clock 분산 패턴)
- 전제: SO_REUSEPORT 등으로 연결이 코어 간 균등 분산됨. 특정 코어에 연결이 편중되면 해당 코어의 per-core 한도에서 조기 차단될 수 있으며, 이는 의도된 보수적 동작이다.
- 메모리 관리:
  - TTL: 윈도우 크기 × 2 (만료 후 자동 정리)
  - 최대 항목 수: 65,536 (config, 초과 시 LRU 퇴출)
  - 만료 정리: TimingWheel (기존 core 인프라 활용)

**요청 파이프라인:**

```
요청 → TLS → [Per-IP] → JWT 검증 → [Per-User] → [Per-Endpoint] → msg_id 라우팅 → Kafka
```

---

## 4. Auth Service 설계 (v0.5.3.0)

### 4.1 인증 플로우 (Kafka 기반 요청-응답)

모든 서비스 간 통신이 Kafka를 경유하는 MSA 원칙을 인증에도 동일하게 적용한다. Gateway가 Auth Service를 직접 호출(HTTP/gRPC)하면 서비스 간 직접 의존이 생기고, 다른 서비스와 통신 방식이 달라진다.

```
1. Client → Gateway: LoginRequest (WireHeader)
2. Gateway: correlation_id 생성 → Pending Requests Map에 등록
3. Gateway → Kafka `auth.requests`: Kafka Envelope (corr_id 포함)
4. Auth Service: consume → PostgreSQL 자격 증명 검증 → JWT 발급 → Redis 세션 저장
5. Auth Service → Kafka `auth.responses`: 응답 Envelope (원본 corr_id)
6. Gateway: consume → corr_id로 Pending Map 조회 → 원래 세션에 응답 전달
7. Gateway → Client: LoginResponse (WireHeader)
```

### 4.2 Pending Requests Map

**core_id를 Metadata에 별도 필드로 보유 (corr_id와 분리)**

- corr_id는 순수 고유 ID (monotonic counter), 비트 조작 없음
- core_id는 u16 (65,536코어 지원 — 듀얼/쿼드 소켓 서버 대응)

**Pending Map: per-core 소유**

```
[요청 시 — Core 2에서 처리]
corr_id = generate_unique_id()
metadata.core_id = 2
pending_maps_[2][corr_id] = {session, timeout}

[응답 수신 — Kafka consumer가 Core 0에서 받은 경우]
// 의사코드이며 실제 cross_core_call API 시그니처에 맞게 조정
target_core = metadata.core_id  // 2
cross_core_call(core_2, deliver_response, corr_id)

[Core 2에서]
pending_maps_[2].find(corr_id) → session → enqueue_write
```

- 글로벌 lock 제로 (shared-nothing 유지)
- cross-core call은 기존 인프라 활용
- **TimingWheel 기반 타임아웃**: 개별 타이머 대신 TimingWheel로 만료된 요청을 배치 정리
- 타임아웃 시 클라이언트에 시스템 에러 응답 (GatewayError::SERVICE_TIMEOUT)

### 4.3 JWT 토큰 갱신

**결정: Refresh Token 방식 (A안).**

Silent refresh(토큰 자동 갱신)는 클라이언트 복잡도를 높이고, Sliding session은 토큰 수명 관리가 모호해진다. Refresh Token 방식이 가장 명확하고 업계 표준.

- **Access Token**: 15~30분 (짧은 수명 → 탈취 피해 최소화)
- **Refresh Token**: 7~30일 (장기 세션 유지)
- 만료 흐름:
  1. Access Token 만료 → 클라이언트가 `RefreshTokenRequest` (시스템 msg_id 범위) + Refresh Token 전송
  2. Auth Service가 Refresh Token 유효성 검증 → 새 Access Token 발급
  3. Refresh Token도 만료 → 재로그인 필요

---

## 5. Chat Service 설계 (v0.5.3.1)

### 5.1 기능 범위

| 기능 | 설명 | 검증하는 메시지 경로 |
|------|------|---------------------|
| 방 입장/퇴장 | 채팅방 참여 관리 | 요청-응답 |
| 메시지 전송/수신 | 방 내 메시지 브로드캐스트 | 브로드캐스트 (채널) |
| 방 목록 조회 | 활성 방 리스트 | 요청-응답 |
| 방 생성 | 새 채팅방 생성 | 요청-응답 |
| 귓속말 (1:1) | 개인 메시지 | 단일 전달 |
| 채팅 히스토리 | 과거 메시지 조회 | 요청-응답 + DB |
| 전역 브로드캐스트 | 접속자 전체 공지 | 전역 브로드캐스트 |

### 5.2 브로드캐스트 아키텍처

**결정: Redis Pub/Sub 외부화.**

Gateway 내장 pub/sub은 Gateway 인스턴스 간 메시지 동기화가 안 된다. 수평 확장 시 Gateway가 N대가 되면 모든 인스턴스가 동일한 메시지를 수신해야 하는데, 이를 위해 어차피 외부 메시지 브로커가 필요하다. 처음부터 Redis Pub/Sub으로 가면 Gateway 수평 확장이 자연스럽다.

**메시지 흐름:**

```
Service → Redis PUBLISH("pub:chat:room:{id}", msg)
         ↓
모든 Gateway 인스턴스 → Redis SUBSCRIBE → 로컬 세션에 전달 + cross-core call
```

**설계 원칙:**

- Gateway는 **채널명의 의미를 모름** (opaque string) — `pub:chat:room:42`든 `pub:match:lobby:1`이든 동일하게 처리
- 구독/해제: 서비스가 **제어 메시지**로 Gateway에 지시 (Gateway가 스스로 판단하지 않음)
- 전역 브로드캐스트: `pub:global:chat` 채널 (Gateway 시작 시 자동 구독)

이 설계로 Gateway 코드 변경 없이 어떤 서비스든 브로드캐스트 기능을 사용할 수 있다.

**Redis Pub/Sub listener: 전용 스레드**

- Gateway 시작 시 Pub/Sub 전용 스레드 생성 (io_context 독립)
- 이 스레드가 Redis SUBSCRIBE 관리 (채널 추가/제거)
- 메시지 수신 시: 채널 → 세션 목록 조회 (읽기 전용 구조, RCU 또는 per-core 복제)
- 세션이 속한 core로 cross_core_call fan-out
- fan-out 비용: 코어 N개 × cross_core_call 1회씩 = N회 (코어 수에 비례, 실용적으로 문제 없음)

### 5.3 메시지 경로 정리

Wave 2에서 구현하는 세 가지 메시지 경로:

| 경로 | 패턴 | 예시 |
|------|------|------|
| 요청-응답 | Client → GW → Kafka → Service → Kafka → GW → Client | 로그인, 방 입장, 히스토리 조회 |
| 브로드캐스트 | Service → Redis PUBLISH → 모든 GW SUBSCRIBE → 로컬 세션 | 채팅 메시지, 공지 |
| 히스토리 저장 | 실시간: Redis Pub/Sub → Gateway → Client / 영구: 별도 Kafka consumer(DB writer)가 동일 토픽 소비 → PostgreSQL 저장 | 채팅 로그 영구 보존 (이중 경로) |

### 5.4 게임플레이 직접 연결 (향후)

Wave 2는 **로비 계층**(Kafka 경유)에 집중한다. 실시간 게임플레이(프레임 단위 동기화)는 Kafka 지연이 허용되지 않으므로 별도 직접 연결이 필요하다.

```cpp
// 향후 게임플레이 서버 — 프레임워크 수정 없이 확장
server.listen<GameProtocol, PlainTcpTransport>(9100);
```

Protocol concept + Transport concept 덕분에 `listen` 호출 하나로 완전히 다른 프로토콜의 서버를 같은 프로세스에서 구동할 수 있다. 이것이 Wave 1에서 concept 기반으로 설계한 핵심 이유.

---

## 6. 에러 처리

### 6.1 방식: msg_id 스코프 에러 enum (응답 메시지에 내장)

중앙 에러 레지스트리 방식은 서비스 간 의존성을 만들고, 에러 코드 번호 충돌 관리가 필요하다. 각 응답 메시지가 자체 에러 enum을 보유하면:

- 에러 레지스트리 불필요 — 서비스별 자유 정의
- 에이전트가 완전 자율적으로 메시지를 설계할 수 있음
- FlatBuffers의 default value와 자연스럽게 결합

### 6.2 서비스 응답 에러 예시

```fbs
enum LoginError : uint16 { NONE = 0, BAD_CREDENTIALS = 1, ACCOUNT_LOCKED = 2 }
table LoginResponse {
  error: LoginError = NONE;
  token: string;
  user_id: uint64;
}
```

### 6.3 Gateway(서비스 0) 에러

Gateway는 시스템 msg_id 범위(0~999)에서 범용 에러를 반환한다.

```fbs
enum GatewayError : uint16 {
  NONE = 0,
  RATE_LIMITED_IP = 1,
  RATE_LIMITED_USER = 2,
  RATE_LIMITED_ENDPOINT = 3,
  JWT_EXPIRED = 11,
  JWT_INVALID = 12,
  JWT_BLACKLISTED = 13,
  SERVICE_TIMEOUT = 21,
  SERVICE_UNAVAILABLE = 22,
  INVALID_MSG_ID = 31,
}
table SystemResponse {
  error: GatewayError = NONE;
  message: string;
  retry_after_ms: uint32;
  limited_msg_id: uint32;  // Rate Limit 시 제한된 msg_id 반환 (향후 추가 가능)
}
```

### 6.4 클라이언트 에러 처리 흐름

```
응답 수신 → flags 에러 비트 확인
  ├─ msg_id ∈ [0, 999] → SystemResponse로 파싱 → GatewayError 처리
  └─ msg_id ≥ 1000    → 해당 msg_id의 Response 타입으로 파싱 → error 필드 확인
```

모든 서비스가 동일한 흐름, 분기 없음. 새 서비스 추가 시 클라이언트 에러 처리 코드 수정 불필요.

---

## 7. 데이터 레이어

### 7.1 Kafka 설계

| 항목 | 설계 | 근거 |
|------|------|------|
| 토픽 | 서비스별 요청/응답 토픽 + DLQ | 서비스 격리, 독립 스케일링 |
| 메시지 키 | session_id 기본, 컨텍스트별 키 전환 가능 | 파티션 균등 분산 + 동일 세션 순서 보장 |
| 파티션 전략 | session_id 해싱 (기본) | msg_id 기반은 특정 메시지 타입(ChatSendMessage 등)에 핫스팟 발생. session_id로 고르게 분산하되, 컨텍스트에 따라 키 전환 가능 (예: 방 입장 후 room_id로 변경) |

**키 전환 책임:**

키 전환은 서비스가 Kafka produce 시 결정한다 (Gateway가 아님). 예: Chat Service가 방 내 메시지를 produce할 때 room_id를 Kafka 키로 설정. Gateway는 클라이언트 → Kafka 구간에서 session_id를 키로 사용하며, 서비스 간 Kafka 통신의 키 결정에는 관여하지 않는다. 이 원칙은 Gateway 도메인 무의존 원칙(section 3.1)과 일관된다.

### 7.2 Redis 설계 — 물리적 4분리

각 Redis 인스턴스가 완전히 다른 트래픽 패턴을 가지므로 물리적으로 분리한다. 장애 격리, 독립 스케일링, 트래픽 특성별 최적화(persistence, eviction 정책 등)가 가능.

| 인스턴스 | 용도 | 트래픽 특성 |
|----------|------|------------|
| Redis #0 | Auth 전용 (세션, JWT 블랙리스트) | 읽기 위주, TTL 기반 자동 만료 |
| Redis #1 | Gateway 전용 (Rate Limit 카운터) | 쓰기 집중, 높은 TPS, 짧은 TTL |
| Redis #2 | Chat 데이터 (방 멤버십, 접속 상태) | 읽기/쓰기 혼합 |
| Redis #3 | Pub/Sub 전용 (브로드캐스트) | 메시지 버스, persistence 불필요 |

각 인스턴스 내에서도 DB 번호로 논리 격리 적용.

### 7.3 PostgreSQL 설계 — 물리 1개 + 스키마 분리

현 규모에서 DB를 물리 분리하면 운영 비용만 증가한다. 스키마 분리로 논리적 격리를 확보하되, 물리 분리로의 전환 경로를 열어둔다.

| 스키마 | 서비스 | 주요 테이블 |
|--------|--------|------------|
| `auth_svc` | Auth Service | users, refresh_tokens |
| `chat_svc` | Chat Service | rooms, messages |

**격리 규칙:**

- 서비스별 전용 DB 롤 + 권한 격리 (cross-schema 접근 원천 차단)
- connection string을 config로 추상화 → 물리 분리 시 코드 변경 제로
- 서비스 간 데이터 필요 시 **비정규화** (JOIN 금지 — MSA 원칙)

**향후 진화**: 핫 패스 물리 분리 (Auth Read Replica) → 완전 분리.

### 7.4 FlatBuffers 스키마 관리

**결정: 공통 + 서비스별 분리 (C안).**

```
apex_shared/schemas/common/     ← envelope, error, common types (Wave 2에서 신규 생성)
apex_services/{service}/schemas/ ← 서비스 전용 메시지
```

- 공통 스키마는 모든 서비스가 의존, 서비스별 스키마는 해당 서비스만 소유
- Gateway는 공통 스키마(envelope)만 알면 됨 — 서비스 메시지 내용은 opaque

### 7.5 msg_id 버저닝 규칙

| 변경 유형 | msg_id 처리 | 예시 |
|-----------|-------------|------|
| 필드 추가, deprecated, 기본값 변경 | 같은 msg_id 유지 | LoginResponse에 `display_name` 필드 추가 |
| 필드 타입 변경, 의미 변경, 구조 재설계 | **새 msg_id 할당** | user_id를 u32 → u64로 변경 |
| msg_id 퇴역 | **영원히 재사용 금지** | — |

**관리 체계:**

- `msg_registry.toml` 중앙 관리 — coordinator가 msg_id 할당 독점
- 에이전트는 `.fbs` 스키마만 작성
- auto-review (reviewer-api)가 규칙 준수 자동 검증
- 에이전트 자율 개발 유지, 사용자 개입 불필요

이 규칙은 CLAUDE.md와 auto-review에 모두 반영하여 강제한다.

---

## 8. 설계 결정 요약

| # | 주제 | 결정 | 핵심 근거 |
|---|------|------|-----------|
| 1 | 스코프 | A안 풀 패키지 | 파이프라인 전체 경로를 한 번에 검증 |
| 2 | Logic Service | Chat 채택 | 모든 메시지 경로 + 모든 어댑터를 하나로 검증 |
| 3 | WireHeader msg_id | u16 → u32 확장 | 마지막 변경 기회 + retired 재사용 금지 정책 |
| 4 | TLS | Gateway 내부 직접 처리 | 단일 바이너리 배포, 외부 의존 제거 |
| 5 | Transport | concept 분리 (TLS ≠ Protocol) | 관심사 분리, per-core 컨텍스트 |
| 6 | JWT 라이브러리 | jwt-cpp | header-only, OpenSSL 재활용 |
| 7 | JWT 블랙리스트 | C안: 로컬 검증 + 민감 작업만 Redis | hot path 네트워크 비용 제로 |
| 8 | msg_id 라우팅 | 범위 기반 + 이진 탐색 | Gateway 도메인 무의존 |
| 9 | Rate Limiting | Sliding Window Counter 3계층 | boundary burst 방지, 처음부터 올바르게 |
| 10 | 인증 플로우 | Kafka 경유 요청-응답 | MSA 통신 방식 통일 |
| 11 | 토큰 갱신 | Refresh Token (A안) | 명확한 수명 관리, 업계 표준 |
| 12 | 브로드캐스트 | Redis Pub/Sub 외부화 | Gateway 수평 확장 자연스러움 |
| 13 | 에러 처리 | msg_id 스코프 enum (방식 2) | 중앙 레지스트리 불필요, 자율 개발 |
| 14 | Redis | 물리 4분리 | 장애 격리 + 트래픽 특성별 최적화 |
| 15 | PostgreSQL | 물리 1 + 스키마 분리 | 현 규모 적정 + 진화 경로 확보 |
| 16 | 스키마 관리 | 공통 + 서비스별 분리 (C안) | Gateway는 envelope만 인지 |
| 17 | msg_id 관리 | msg_registry.toml 중앙 + auto-review 강제 | 에이전트 자율성 유지하면서 충돌 방지 |

---

## 9. 향후 확장 (Wave 2 이후)

| 항목 | 현재 (Wave 2) | 향후 |
|------|---------------|------|
| 설정 변경 | FileWatcher TOML hot-reload | Admin API (8081) → Dashboard |
| 브로드캐스트 | Redis Pub/Sub | 규모에 따라 NATS 전환 가능 (어댑터 추상화) |
| 게임플레이 | 미구현 (로비 계층만) | `listen<GameProtocol, PlainTcpTransport>` 직접 연결 |
| DB 스케일링 | PostgreSQL 물리 1개 | Auth Read Replica → 완전 분리 |
| Rate Limit 알고리즘 | Sliding Window Counter | 필요시 교체 가능 (어댑터 패턴) |
| Admin 보안 | 내부망 격리 | API Key → mTLS |
| Idempotency Key | 미구현 | Wave 2 범위 외, v0.6에서 검토 |

### 9.1 머지 전 필수 갱신 (Apex_Pipeline.md)

- §9 Server API 예시: `listen<P>` → `listen<P, T>` 시그니처 변경
- §5 안정성 설계: Rate Limiting "Token Bucket, Redis 기반 분산" → "Sliding Window Counter 3계층 (Per-IP: Gateway 로컬 메모리, Per-User/Per-Endpoint: Redis Lua 스크립트)" 변경
- WireHeader 레이아웃: 10바이트 → 12바이트, msg_id u16 → u32

### 9.2 Auth 도메인 상세

비밀번호 해싱(bcrypt/argon2), 계정 잠금 정책 등은 Auth Service 상세 설계에서 별도 정의한다.

### 9.3 msg_registry.toml 초기 구조

시스템 msg_id 초기 할당 예시:

```toml
[system]
range = [0, 999]
allocated = [
    { id = 1, name = "Heartbeat", status = "active" },
    { id = 2, name = "SystemErrorResponse", status = "active" },
    { id = 10, name = "RefreshTokenRequest", status = "active" },
    { id = 11, name = "RefreshTokenResponse", status = "active" },
]
```

### 9.4 E2E 최소 검증 시나리오

- 로그인 → JWT 발급 → 인증된 요청 → 응답
- 채팅방 입장 → 메시지 전송 → 브로드캐스트 수신
- 전역 브로드캐스트
- JWT 만료 → Refresh Token 갱신
- Rate Limit 초과 → 에러 응답
- 서비스 타임아웃 → Gateway 에러

---

## 10. 구현 운영 규칙

### 10.1 자동화 범위

- 구현 계획(Plan 0~5) 확정 후 **전 과정 에이전트 자율 실행** — 사용자 승인 불필요
- 포함 범위: 구현, 빌드, 테스트, auto-review, 이슈 자체 수정, PR 생성, 머지, 워크트리 정리

### 10.2 PR / 머지

- E2E 통과 + 문서 갱신 완료 후 자동 진행
- `gh pr create` → `gh pr merge --squash --admin --delete-branch`
- 사용자 승인 불필요

### 10.3 워크트리 정리

- 머지 완료 후 `apex_tools/cleanup-branches.sh --execute`로 일괄 정리
- 수동 3점 정리 불필요 (스크립트가 worktree remove + branch -D + push --delete 처리)

### 10.4 Critical 설계 이슈 대응

- auto-review에서 설계 변경이 필요한 Critical 이슈 발견 시:
  - 구현은 현재 설계대로 **계속 진행** (블로킹 금지)
  - `docs/Apex_Pipeline.md` 백로그 최상단에 **Critical 섹션**으로 기록
  - 다음 세션에서 사용자와 논의 후 대응

### 10.5 E2E 테스트 이슈 대응

- E2E 테스트에서 발견된 이슈는 **즉시 수정** (블로킹 금지와 별개)
- 원인 분석 → 수정 → 단위 테스트 추가 → E2E 재실행 → 통과 확인
- Critical 설계 이슈(§10.4)와 다름 — 구현 버그는 에이전트가 자체 해결

### 10.6 Plan별 실행 순서

```
Plan 0 (기반) → Plan 1 (Gateway) → Plan 2 (Rate Limit)  ← 순차
                                  → Plan 3 (Auth)        ← Plan 1 이후 병렬 가능
                                  → Plan 4 (Chat)        ← Plan 3 이후
                                  → Plan 5 (E2E + 문서)  ← 전부 이후
```

### 10.7 커밋 규칙

- Task 단위 커밋: `feat(scope): 한국어 설명`
- Plan 단위 auto-review 보고서: `docs(wave2): Plan N auto-review 보고서`
- 문서 갱신: `docs: 마스터 문서 Wave 2 반영`
