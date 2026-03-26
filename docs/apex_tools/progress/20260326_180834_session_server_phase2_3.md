# Progress: 세션 서버 Phase 2-3 (ConPTY + 대시보드 /branches)

> **브랜치**: feature/session-server-conpty
> **백로그**: BACKLOG-238 (워크스페이스+세션), BACKLOG-239 (FIX UI+blocked badge)
> **신규 백로그**: BACKLOG-243 (BranchInfo 필드 미채움), BACKLOG-244 (Windows graceful shutdown)

## 완료 내용

### Phase 2: ConPTY 독립 프로세스 세션 서버

- **세션 서버** (`:7601`): 데몬과 별도 프로세스로 실행, ConPTY 기반 가상 터미널 할당
- **WebSocket 핸들러**: `/ws/{workspace_id}` 엔드포인트, xterm.js 실시간 양방향 통신
- **HTTP API**: 세션 CRUD (`/api/sessions`), 텍스트 전송 (`/api/sessions/{id}/send`)
- **Watchdog**: 세션 서버 프로세스 PID 추적, 비정상 종료 시 자동 재시작
- **프로세스 관리**: 데몬이 `session run`으로 자식 프로세스 기동, 데몬 종료 시 함께 종료
- **CLI**: `session run/start/stop/status/send` 5개 커맨드

### Phase 3: 대시보드 /branches 페이지

- **Branches 페이지**: 워크스페이스 목록 + git 상태 + 세션 상태 통합 표시
- **xterm.js 웹 터미널**: WebSocket 연결로 세션 출력 실시간 스트리밍
- **세션 제어 UI**: 시작/중지 버튼, 상태 표시
- **리버스 프록시**: 메인 대시보드(`:7600`)가 `/session/` 경로를 세션 서버(`:7601`)로 프록시
- **blocked_reason 뱃지**: 네비 바 + Backlog 페이지에 ⚠ 뱃지 표시
- **Workspace REST API**: `/api/workspace`, `/api/workspace/{id}`, `/api/workspace/scan`
- **대시보드 5개 페이지 체제**: Dashboard, Backlog, Handoff, Queue, Branches

### 설정

- `[session]` config 활성화: `enabled`, `addr`, `watchdog_interval`, `output_buffer_lines`
- `[workspace]` config 연동: 스캔 결과를 대시보드 + 세션 서버에서 공유

## 리뷰 결과

- auto-review 수행, 이슈 수정 완료

## 잔여 이슈

잔여 이슈: BACKLOG-243, BACKLOG-244로 이관 완료.
