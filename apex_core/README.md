# apex_core

C++23 코루틴 기반 고성능 TCP 서버 프레임워크.
Boost.Asio 위에 io_context-per-core shared-nothing 아키텍처(Seastar 모델)를 구현하며,
CRTP 정적 다형성으로 런타임 오버헤드를 최소화한다.

## 컴포넌트

### 기반

| 이름 | 설명 |
|------|------|
| MpscQueue | 락프리 bounded MPSC 큐 |
| SlabPool | O(1) 슬랩 메모리 풀 |
| RingBuffer | zero-copy 수신 버퍼 (linearize 지원) |
| TimingWheel | O(1) 타임아웃 관리 |

### 프레임워크

| 이름 | 설명 |
|------|------|
| CoreEngine | io_context-per-core 멀티코어 엔진 |
| MessageDispatcher | O(1) 메시지 디스패치 (코루틴 핸들러) |
| ServiceBase | CRTP 서비스 베이스 클래스 |
| WireHeader | 10바이트 고정 헤더 (big-endian) |
| FrameCodec | RingBuffer 기반 프레임 코덱 |

### 프로토콜 / 네트워크

| 이름 | 설명 |
|------|------|
| ProtocolBase | CRTP 프로토콜 추상화 |
| TcpBinaryProtocol | FrameCodec 래핑 구현체 |
| Session / SessionManager | TCP 세션 관리 + 하트비트 |
| TcpAcceptor | 코루틴 accept loop |
| Server | 최상위 통합 (ServerConfig 기반) |

## 빌드

Windows 10 / VS2022 / C++23, CMake + Ninja + vcpkg.

```bash
build.bat debug    # 디버그 빌드
build.bat default  # 릴리스 빌드
```

출력: `bin/{target}_{variant}.exe`

## 의존성 (현재 사용 중)

benchmark, boost-asio, boost-unordered, flatbuffers, gtest, spdlog, tomlplusplus

> 향후 추가 예정: librdkafka + KafkaSink (v0.4.1), redis-plus-plus (v0.4.2), libpq (v0.4.3), prometheus-cpp (v0.6.1), jwt-cpp (v0.5.3), boost-beast (v0.5.1)

## 문서

설계 문서 및 ADR은 `docs/` 디렉토리 참조.
