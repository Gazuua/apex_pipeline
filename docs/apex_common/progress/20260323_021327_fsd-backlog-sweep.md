# FSD Backlog Sweep — 백로그 소탕 완료

**날짜**: 2026-03-23
**브랜치**: feature/fsd-backlog-20260323_011850
**PR**: #117

## 해결 항목 (6건)

| # | 항목 | 타입 | 핵심 변경 |
|---|------|------|-----------|
| #139 | is_account_locked 타임존 비교 | BUG | SQL `locked_until > NOW()` 전환, auth_logic.hpp 삭제 |
| #140 | TcpAcceptor bind_address | SECURITY | ServerConfig.bind_address 추가, make_address 파싱 |
| #141 | TCP max_connections | SECURITY | ServerConfig.max_connections, Listener에서 enforce |
| #143 | spawn_adapter_coro 테스트 | TEST | DRAINING/CLOSED 거부 테스트 3건 추가 |
| #144 | Session::state_ atomic | DESIGN_DEBT | std::atomic<State>, relaxed ordering |
| #145 | on_leave_room TOCTOU | DESIGN_DEBT | Lua 스크립트 원자화 |

## 드롭 항목 (FSD 분석 메모 추가)

| # | 항목 | 사유 |
|---|------|------|
| #133 | TransportContext SocketBase | 대규모 리팩터링, 설계 확정되었으나 FSD 범위 초과 |
| #137 | KafkaConsumer 소멸자 | async handler lifetime 타이밍 분석 필요 |
| #142 | CrashHandler 테스트 | fork/subprocess + SEH, 구현 비용 높음 |
| #65 | auto-review 가이드 검증 | Go 백엔드 스코프, 별도 작업 |
| #59 | 문서 자동화 | 5가지 시스템 대규모 인프라 |

## auto-review 수정 (3건)

- make_address: error_code overload + spdlog::critical
- max_connections: active_sessions() 결과 캐싱
- max_connections: socket.close() error_code overload

## 추가 백로그

- **#153**: apex-agent 백로그 동기화 — BACKLOG.md 수정 시 SQLite 미갱신
