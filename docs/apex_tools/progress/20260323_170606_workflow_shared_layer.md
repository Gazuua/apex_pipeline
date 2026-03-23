# workflow 공유 레이어 + BACKLOG-157 완료

**PR**: #119
**브랜치**: `feature/apex-agent-enhancements`

## 작업 결과

### 신규 패키지: `internal/workflow/`

CLI 인라인 로직을 공유 레이어로 추출. HTTP 대시보드에서도 동일 파이프라인 호출 가능.

| 파일 | 함수 | 역할 |
|------|------|------|
| `git.go` | ValidateNewBranch, CreateAndPushBranch, RebaseOnMain, CheckoutMain | git 조작 유틸 |
| `sync.go` | SyncImport, SyncExport | 백로그 MD↔DB 동기화 |
| `pipeline.go` | IPCFunc, StartPipeline, MergePipeline, DropPipeline | 파이프라인 조합 |

### 백로그 자동 동기화

| 이벤트 | 동작 |
|--------|------|
| `notify start` | SyncImport (MD→DB, non-fatal) |
| `notify merge` | RebaseOnMain → SyncImport → SyncExport (전부 fatal) |

### BACKLOG-157 해결

`RebaseOnMain()`에서 rebase 실패 → abort 시도 → abort도 실패 시 경고 로그 + 수동 복구 안내.

### CLI 리팩터링

- `doNotifyStart` → `workflow.StartPipeline()` 호출
- `notifyMergeCmd` → `workflow.MergePipeline()` 호출
- `notifyDropCmd` → `workflow.DropPipeline()` 호출
- `EnforceRebase` → `workflow.RebaseOnMain()` 위임

### auto-review 수정 (4건)

1. MergePipeline에서 CheckoutMain 제거 (uncommitted export 방지)
2. projectRoot 폴백 `"."` → 에러 반환
3. rev-list 실패 시 경고 로그 추가
4. 중복 assertion 제거

### 가이드 보강

- `CLAUDE.md`: notify start 필수 플래그/비백로그 분기, cleanup dry-run
- `apex-agent CLAUDE.md`: cleanup CLI 섹션, 데몬 idle timeout 30분, 데몬 장애 시 대응

### 백로그 추가

- BACKLOG-159: CLI 전체 워크플로우 workflow 패키지 이관 (HTTP 대시보드 기틀)
- BACKLOG-160: UpdateFromImport title 갱신 누락 버그

## 테스트

- 단위 테스트 20개 ALL PASS
- 기존 E2E 테스트 (EnforceRebase 포함) ALL PASS
- CI 2회 PASS (코드 변경 + auto-review 수정 후)
