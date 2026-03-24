# apex-agent 리뷰 후속 개선 설계

## 배경

full review에서 발견된 대규모 이슈 3건(BACKLOG-181, 182, 184)을 이번 PR에 포함시키기 위한 설계 결정.

## BACKLOG-181: context 전파 (A안 — 진입점만)

**결정**: IPC 핸들러가 이미 받는 `ctx`를 Manager → `RunInTx`까지 전달. Manager 내부 private 함수, CLI 직접 호출 경로는 변경하지 않음.

**변경 대상**:
- `handoff/manager.go`: NotifyStart, NotifyTransition, finalizeBranch, NotifyMerge, NotifyDrop에 ctx 파라미터 추가
- `handoff/module.go`: 핸들러 4개에서 ctx 전달
- `queue/manager.go`: TryAcquire, tryPromote에 ctx 파라미터 추가
- `queue/module.go`: handleTryAcquire에서 ctx 전달
- 테스트 파일: 시그니처 변경에 따른 업데이트

**변경하지 않는 곳**: backlog SafeExportJSON (CLI/workflow 직접 호출), IPC client, migrator, daemon lifecycle

## BACKLOG-182: cleanup 외부 명령 타임아웃 (B안 — 2단계)

**결정**: 로컬 git 5초 / 네트워크 명령(gh, git push) 30초

**변경 대상**:
- `cleanup/cleanup.go`: `runGit` → `runGitCtx(ctx, ...)` 도입, `IsMergedToMain`/`blobHashMatch`에 context 파라미터 추가
- `Run()` 최상위에서 2개 context 생성 (localCtx 5s, networkCtx 30s)
- Layer 2 gh CLI 호출에 networkCtx 적용

## BACKLOG-184: 테스트 추가 (1, 2만)

1. **마이그레이션 data integrity**: v1 INSERT → v2 적용 → 데이터 유지 검증
2. **skip_design 상태 전이 단위 테스트**: started 스킵 → 바로 implementing 확인
