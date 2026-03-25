# Progress: notify merge 통합 — 머지 유일 진입점 (BACKLOG-234)

- **브랜치**: feature/notify-merge-integration
- **PR**: #174
- **날짜**: 2026-03-26

## 작업 결과

`handoff notify merge`를 머지의 유일한 진입점으로 통합 완료.

### 변경 사항

1. **MergeFullPipeline** (workflow/pipeline.go) — 데몬이 lock→export→rebase→push→gh pr merge→finalize→checkout을 원자적으로 수행하는 8단계 파이프라인
2. **handleNotifyMerge 확장** (handoff/module.go) — project_root를 받아 MergeFullPipeline 호출. FIXING 백로그 사전 검증 포함
3. **QueueOperator 인터페이스** (handoff/manager.go) — handoff 모듈이 queue 모듈의 lock을 사용
4. **CLI 단순화** (cli/handoff_cmd.go) — `notify merge --summary "..."` 한 줄로 전체 머지 수행
5. **gh pr merge 전면 차단** (cli/hook_cmd.go) — validate-merge hook이 무조건 차단
6. **queue merge 서브커맨드 제거** (cli/queue_cmd.go) — acquire/release/status 제거
7. **auto-review 5건 수정** — finalize 에러 처리, 중간 push 제거, context 안전성, FIXING 사전 검증, timeout 확대

### 테스트

- Go 단위 테스트: ALL PASS (단위 + E2E, -race -cover)
- CI: Go (apex-agent) 잡 성공
