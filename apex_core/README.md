# apex_core

C++23 코루틴 기반 고성능 TCP 서버 프레임워크.
Boost.Asio 위에 io_context-per-core shared-nothing 아키텍처(Seastar 모델)를 구현하며,
CRTP 정적 다형성으로 런타임 오버헤드를 최소화한다.

## 컴포넌트

### 기반

| 이름 | 설명 |
|------|------|
| MpscQueue | 락프리 bounded MPSC 큐 |
| SlabAllocator | O(1) 슬랩 메모리 풀 |
| BumpAllocator | 요청 수명 임시 데이터, 포인터 전진 O(1) |
| ArenaAllocator | 트랜잭션 수명, 블록 체이닝 + 벌크 해제 |
| RingBuffer | zero-copy 수신 버퍼 (linearize 지원) |
| TimingWheel | O(1) 타임아웃 관리 |
| Result | std::expected 기반 에러 핸들링 (Result\<T\>) |
| ErrorCode | 프레임워크 공통 에러 코드 |

### 프레임워크

| 이름 | 설명 |
|------|------|
| CoreEngine | io_context-per-core 멀티코어 엔진 |
| MessageDispatcher | O(1) 메시지 디스패치 (코루틴 핸들러) |
| ServiceBase | CRTP 서비스 베이스 클래스 |
| WireHeader | 12바이트 고정 헤더 v2 (big-endian) |
| FrameCodec | RingBuffer 기반 프레임 코덱 |
| CrossCoreDispatcher | 코어 간 메시지 패싱 (op 기반 handler dispatch) |
| SharedPayload | 코어 간 immutable 데이터 공유 (atomic refcount) |
| AdapterInterface | 외부 어댑터 타입 소거 인터페이스 |
| ConnectionHandler | TCP 연결 처리 (Server에서 분리) |

### 프로토콜 / 네트워크

| 이름 | 설명 |
|------|------|
| Protocol concept | C++20 concept 기반 프로토콜 추상화 (core 정의, shared 구현) |
| TcpBinaryProtocol | FrameCodec 래핑 구현체 (apex_shared에 위치) |
| Session / SessionManager | TCP 세션 관리 + 하트비트 (intrusive_ptr, SlabAllocator 할당) |
| TcpAcceptor | 코루틴 accept loop |
| Server | 최상위 통합 (ServerConfig 기반) |

## 빌드

Windows 10 / VS2022 / C++23, CMake + Ninja + vcpkg.

```bash
build.bat debug    # 디버그 빌드
build.bat release  # 릴리스 빌드
```

출력: `bin/{variant}/{target}.exe` (예: `bin/debug/echo_server.exe`)

## 의존성

**현재 사용 중**: benchmark, boost-asio, boost-beast, boost-unordered, flatbuffers, gtest, hiredis, jwt-cpp, libpq, librdkafka, openssl, redis-plus-plus, spdlog, tomlplusplus

**사용 중**: 자체 MetricsHttpServer (Beast HTTP 기반, v0.6.1.0부터)

## 문서

설계 문서 및 ADR은 `docs/` 디렉토리 참조.
