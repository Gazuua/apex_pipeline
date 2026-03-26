# SocketBase Virtual Interface 도입 — BACKLOG-133 완료

**PR**: #191 | **브랜치**: feature/socket-base-abstraction | **날짜**: 2026-03-26

## 작업 요약

apex_core의 OpenSSL 직접 의존을 해소하기 위해 SocketBase virtual interface를 도입. Session/SessionManager/ConnectionHandler를 비템플릿 상태로 유지하면서 TCP/TLS 소켓을 동일한 인터페이스로 취급할 수 있도록 변경.

## 주요 변경 사항

### 신규 파일
- `apex_core/include/apex/core/socket_base.hpp` — SocketBase 인터페이스 + TcpSocket 구현
- `apex_core/tests/unit/test_socket_base.cpp` — TcpSocket 단위 테스트 8건
- `apex_shared/lib/protocols/tcp/.../tls_socket.hpp` — TlsSocket 구현 (ssl::stream 래핑)
- `docs/apex_common/plans/20260326_105911_socket_base_abstraction.md` — 구현 계획서

### 코어 변경 (23파일, +1366 -145)
1. **SocketBase 계층**: async_read_some, async_write, async_handshake, close, is_open, get_executor, set_option_no_delay, remote_endpoint
2. **Transport concept 확장**: ListenerState, make_listener_state, wrap_socket — Listener가 Transport별 상태를 소유하고 소켓 생성을 위임
3. **Session**: `tcp::socket socket_` → `unique_ptr<SocketBase> socket_`
4. **SessionManager**: `create_session(tcp::socket)` → `create_session(unique_ptr<SocketBase>)`
5. **ConnectionHandler**: `accept_connection(unique_ptr<SocketBase>)` + read_loop 시작 시 `async_handshake()` 호출
6. **Listener**: `T::ListenerState listener_state_` 멤버 추가, accept 콜백에서 `T::wrap_socket()` 사용
7. **Server::listen**: `T::Config transport_config` 파라미터 추가 (기존 API 하위 호환)
8. **CMake**: apex_core에서 OpenSSL::SSL/Crypto 링크 제거

### 테스트/벤치마크
- test_socket_base: 8건 신규 (생성/닫기/멱등/no_delay/handshake/read-write/executor/polymorphic)
- test_session, test_session_write_queue, test_session_manager, test_connection_handler: make_tcp_socket wrap 적용
- bench_session_throughput: Session 의존 제거 (raw TCP 처리량 baseline 측정으로 변경)

## 설계 결정

- **B안 (Virtual SocketBase)** 채택 — FSD 2026-03-22 확정 설계 준수
- **A안 (async_handshake on SocketBase)** — TcpSocket no-op, TlsSocket real handshake
- **Transport concept에 ListenerState/wrap_socket 추가** — Listener 헤더에 SSL include 불필요, 인스턴스화 시점에 호출자가 SSL 링크
- virtual dispatch 비용 ~2ns는 커널 syscall 대비 0.001% 미만으로 무시 가능

## 검증
- MSVC debug 빌드: 경고 0건
- 92/92 테스트 전체 PASS
- CI 3-컴파일러 빌드 대기 중
