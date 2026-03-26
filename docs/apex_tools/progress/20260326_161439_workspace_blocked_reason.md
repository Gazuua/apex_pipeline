# Progress: workspace 모듈 + blocked_reason Phase 1

> **PR**: #195
> **브랜치**: feature/workspace-session-mgmt
> **백로그**: BACKLOG-238 (워크스페이스+세션), BACKLOG-239 (백로그 FIX)

## 완료 내용

### Config 확장
- `[workspace]` 섹션: root, repo_name, scan_on_start
- `[session]` 섹션: enabled, addr, log_dir, watchdog_interval, output_buffer_lines
- Defaults(), Load(), WriteDefault() 모두 반영

### Workspace 모듈 (`internal/modules/workspace/`)
- `local_branches` 테이블 v1 마이그레이션
- CRUD: Upsert, Get, List, Delete, UpdateSession
- 디렉토리 스캐너: workspace root에서 repo_name 접두어 디렉토리 탐색, git branch/status 감지
- IPC 라우트: scan, list, get, sync
- OnStart 자동 스캔 (scan_on_start=true일 때)
- Daemon/TestEnv 통합

### Backlog blocked_reason
- v4 마이그레이션: `ALTER TABLE backlog_items ADD COLUMN blocked_reason TEXT`
- `BacklogItem.BlockedReason` 필드 + 전체 쿼리 반영
- `backlog update --blocked "사유"` / `--blocked ""` CLI
- `DashboardBlockedCount()` 쿼리 (네비 바 뱃지용)

### 테스트
- workspace 단위 테스트 11건 PASS
- backlog blocked 단위 테스트 3건 PASS
- E2E 테스트 2건 (workspace scan/list + backlog blocked) PASS
- 기존 전체 테스트 영향 없음

### 설계 문서
- 전체 설계서: `docs/apex_tools/plans/20260326_152349_workspace_session_mgmt.md`
- Phase 1 구현 계획: `docs/apex_tools/plans/20260326_152349_phase1_foundation.md`

## 후속 작업 (Phase 2-3)
- Phase 2: ConPTY + WebSocket 독립 프로세스 (세션 서버)
- Phase 3: 대시보드 /branches 페이지 + xterm.js + FIX UI
