# apex-agent 로직 정밀 강화 설계

- **날짜**: 2026-03-25
- **브랜치**: feature/agent-logic-hardening
- **목표**: 다중 에이전트 환경에서 워크플로우 신뢰성 + DB 정합성 완벽 보장

## 변경 항목

### 1. StartPipeline SyncImport 제거

**현행**: StartPipeline Phase 4에서 SyncImport(JSON→DB) 호출
**변경**: 제거. import는 SafeExportJSON(import-first)과 수동 복구(migrate backlog)만 유지.
**근거**: import는 백업 복원 용도로 한정되었고, stale guard 등 다양한 방어 패턴이 도입되어 착수 단계 체크 불필요.

### 2. fail-close 전환

**현행**: validate-merge, validate-handoff가 데몬 IPC 실패 시 fail-open(허용)
**변경**: fail-close(차단). handoff-probe의 validate-edit IPC 실패도 fail-close로 통일.
**대상**: hook_cmd.go, handoff_hook_cmd.go
**에러 메시지**: "error: daemon unreachable — run 'apex-agent daemon start'"

### 3. branch_backlogs 소유권 정리

**현행**: backlog.Fix()가 branch_backlogs에 직접 INSERT. Release()는 junctionCleaner 콜백 사용 — 비대칭.
**변경**: Fix()도 junctionCreator 콜백 패턴으로 통일. handoff 모듈이 콜백 주입.
**대상**: backlog/manage.go, handoff 모듈 초기화 경로

### 4. CleanupStale TX 감싸기

**현행**: SELECT → IsProcessAlive → DELETE가 TX 없이 실행
**변경**: 단일 RunInTx로 감싸기. queue_history 이벤트도 TX 내에서 기록.
**대상**: queue/manager.go CleanupStale

### 5. NotifyStart TOCTOU 에러 메시지 개선

**현행**: SetStatusWith FIXING 실패 시 "not found or already FIXING" — 원인 구분 불가
**변경**: RowsAffected=0 시 Get()으로 현재 상태 조회, 분기 메시지 제공
**대상**: backlog/manage.go SetStatusWith

### 6. 워크플로우 가이드 문서

**경로**: docs/apex_tools/apex_agent_workflow_guide.md
**내용**: 7단계별 기능 매핑, 상태 머신 전이 테이블, Hook 게이트 매트릭스, 동시성 시나리오, 데이터 정합성 보증
