# 대시보드 관찰 전용화 — 세션 서버 + Branches 페이지 + local_branches 제거

- **백로그**: BACKLOG-260
- **PR**: #213
- **브랜치**: `feature/dashboard-observe-only`
- **일시**: 2026-03-27

## 작업 요약

HTMX innerHTML swap과 xterm.js 터미널의 근본적 비호환(DOM 파괴, 포커스 유실, 크기 불일치 reflow)으로 인한 디자인 부채를 청산. 대시보드를 관찰 전용(4개 페이지)으로 전환.

## 제거 항목

| 카테고리 | 내용 |
|----------|------|
| session/ | ConPTY, WebSocket, watchdog, manager, server, terminal (9파일) |
| workspace/ | local_branches 테이블, scanner, manager, module (4파일) |
| httpd | branches.go, proxy.go, terminal.js, branches 템플릿 2개 |
| CLI | session_cmd.go (run/start/stop/status/send) |
| E2E | session_test.go, workspace_test.go |
| config | WorkspaceConfig, SessionConfig 섹션 |
| settings.json | SessionEnd hook |
| 의존성 | gorilla/websocket |

## 수정 항목

- httpd: Server.New() 시그니처 7→4인자, WorkspaceQuerier 인터페이스 제거, routes/render에서 branches 제거
- daemon_cmd.go: workspace 모듈 등록 해제
- layout.html: Branches 네비 링크 + blocked-badge 폴링 제거
- plugin_cmd.go: teardown no-op화, setup에서 session registration 제거
- config_cmd.go: workspace/session show 제거
- TestBacklog_BlockedReason_E2E → backlog_test.go 이전

## 결과

- 41 files changed, +95 / -3,508 lines
- 대시보드: 5페이지 → 4페이지 (Dashboard, Backlog, Handoff, Queue)
- Go 빌드 + 전체 테스트 (20 패키지) 통과
- auto-review: gofmt 이슈 1건 수정
