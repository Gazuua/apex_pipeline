# apex-agent 워크플로우 공유 레이어 + BACKLOG-157 설계

## 개요

apex-agent handoff CLI의 인라인 로직을 공유 레이어로 추출하여 CLI와 미래 HTTP 대시보드 모두 동일한 워크플로우를 호출할 수 있도록 한다. 동시에 백로그 import/export 자동화와 BACKLOG-157 rebase abort 에러 핸들링을 포함한다.

## 동기

### 문제 1: 백로그 동기화 갭

- `notify start` 시 import 없음 → DB에 최신 MD 메타데이터 미반영
- `notify merge` 시 export 없음 → MD에 DB 상태 미반영, export 없이 머지 가능
- 에이전트 수동 실행에 의존 → 빠뜨리는 케이스 발생

### 문제 2: CLI 인라인 로직

- git 조작(브랜치 확인, checkout -b, checkout main, rebase)이 CLI 커맨드에 직접 작성
- HTTP 대시보드(BACKLOG-146) 추가 시 동일 로직 중복 구현 필요

### 문제 3: BACKLOG-157

- `EnforceRebase()`에서 rebase 실패 시 `--abort` 호출의 에러를 무시 (`//nolint:errcheck`)
- abort 실패 시 반쪽짜리 rebase 상태에 빠질 위험

## 설계

### 패키지 구조

```
internal/workflow/           ← 신규 패키지
├── git.go                   ← git 조작 유틸
├── sync.go                  ← 백로그 동기화
└── pipeline.go              ← 파이프라인 조합
```

### git.go — git 조작 유틸

```go
package workflow

// ValidateNewBranch checks that branchName does not exist locally or remotely.
// Returns error if the branch already exists.
func ValidateNewBranch(branchName string) error

// CreateAndPushBranch creates a local branch and pushes with upstream tracking.
// Equivalent to: git checkout -b <name> && git push -u origin <name>
func CreateAndPushBranch(branchName string) error

// RebaseOnMain fetches origin/main and rebases the current branch.
// On conflict: aborts rebase (with error logging) and returns error.
// On abort failure: logs warning with current git state for manual recovery.
// This also fixes BACKLOG-157 (rebase abort error handling).
// Returns (message, error). Empty message if already up-to-date.
func RebaseOnMain(projectRoot string) (string, error)

// CheckoutMain switches to main and pulls latest.
func CheckoutMain(projectRoot string) error
```

### sync.go — 백로그 동기화

```go
package workflow

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"

// SyncImport reads BACKLOG.md + BACKLOG_HISTORY.md from projectRoot and
// imports into DB via mgr.ImportItems(). Idempotent — existing items keep
// their status, only metadata is updated.
// Returns the number of newly imported items.
func SyncImport(projectRoot string, mgr *backlog.Manager) (int, error)

// SyncExport runs SafeExport (import-first + export in single TX) and
// writes the result to docs/BACKLOG.md in projectRoot.
// Returns the number of items synced during import-first phase.
func SyncExport(projectRoot string, mgr *backlog.Manager) (int, error)
```

### pipeline.go — 파이프라인 조합

IPC 호출을 콜백으로 추상화하여 CLI/HTTP 모두 동일 파이프라인 사용.

```go
package workflow

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"

// IPCFunc is the abstraction for notify IPC calls.
// CLI: sendHandoffRequest() wrapper
// HTTP: direct Manager method call wrapper
type IPCFunc func(action string, params map[string]any) (map[string]any, error)

// StartPipeline orchestrates the full notify-start workflow:
//   1. ValidateNewBranch(branchName)
//   2. ipcFn("notify-start", params) → DB TX
//   3. CreateAndPushBranch(branchName)
//   4. SyncImport(projectRoot, mgr)
//
// Phase 4 (SyncImport) failure is non-fatal — logs warning, does not block start.
func StartPipeline(branchName string, params map[string]any,
    projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) (notifID float64, err error)

// MergePipeline orchestrates the full notify-merge workflow:
//   1. RebaseOnMain(projectRoot)        — 최신 main 반영
//   2. SyncImport(projectRoot, mgr)     — rebase 후 최신 MD → DB
//   3. SyncExport(projectRoot, mgr)     — DB → MD 파일 쓰기
//   4. ipcFn("notify-merge", params)    — DB TX (FIXING 체크 + finalize)
//   5. CheckoutMain(projectRoot)
//
// Phase 1-3 failure is fatal — 동기화 실패 시 머지 진행 불가.
func MergePipeline(params map[string]any,
    projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) error

// DropPipeline orchestrates the full notify-drop workflow:
//   1. ipcFn("notify-drop", params) → DB TX (FIXING 체크 + finalize)
//   2. CheckoutMain(projectRoot)
func DropPipeline(params map[string]any,
    projectRoot string, ipcFn IPCFunc) error
```

### 호출 관계

```
CLI (handoff_cmd.go)              HTTP (미래 대시보드)
┌────────────────────┐           ┌────────────────────┐
│ doNotifyStart()    │           │ handleStart()      │
│   ipcFn = IPC래퍼   │           │   ipcFn = Manager래퍼│
│   pipeline.Start   │           │   pipeline.Start   │
│     Pipeline()     │           │     Pipeline()     │
├────────────────────┤           ├────────────────────┤
│ notifyMergeCmd()   │           │ handleMerge()      │
│   ipcFn = IPC래퍼   │           │   ipcFn = Manager래퍼│
│   pipeline.Merge   │           │   pipeline.Merge   │
│     Pipeline()     │           │     Pipeline()     │
├────────────────────┤           ├────────────────────┤
│ notifyDropCmd()    │           │ handleDrop()       │
│   ipcFn = IPC래퍼   │           │   ipcFn = Manager래퍼│
│   pipeline.Drop    │           │   pipeline.Drop    │
│     Pipeline()     │           │     Pipeline()     │
└────────────────────┘           └────────────────────┘
         │                                │
         └────────── 동일한 workflow 패키지 ─┘
```

### notify design / plan

IPC 호출 1개만 수행하므로 파이프라인으로 추출하지 않는다. CLI에서 직접 IPC 호출 유지.

### BACKLOG-157: rebase abort 에러 핸들링

`RebaseOnMain()` 구현 시 기존 `EnforceRebase()`의 rebase 코어 로직을 추출하면서 해결.

```go
// RebaseOnMain 내부 — abort 실패 시 처리
rebaseCmd := exec.Command("git", "-C", projectRoot, "rebase", "origin/main", "--quiet")
if err := rebaseCmd.Run(); err != nil {
    abortCmd := exec.Command("git", "-C", projectRoot, "rebase", "--abort")
    if abortErr := abortCmd.Run(); abortErr != nil {
        ml.Warn("rebase --abort 실패 — 수동 복구 필요",
            "rebase_err", err, "abort_err", abortErr)
        return fmt.Errorf("차단: rebase 충돌 + abort 실패. 수동 복구 필요:\n"+
            "  git rebase --abort  (또는 git rebase --continue)\n"+
            "  rebase 에러: %v\n  abort 에러: %v", err, abortErr)
    }
    return fmt.Errorf("차단: origin/main rebase 중 충돌 발생.\n"+
        "  수동으로 해결 후 다시 시도하세요:\n"+
        "  git fetch origin main && git rebase origin/main")
}
```

### EnforceRebase 리팩터링

기존 `hook/rebase.go`의 `EnforceRebase()`는 push/pr 트리거 판정 + rebase를 함께 수행. 리팩터링 후 트리거 판정만 담당하고, 실제 rebase는 `workflow.RebaseOnMain()` 위임.

```
hook/rebase.go (기존)              리팩터링 후
┌───────────────────────┐        ┌───────────────────────┐
│ EnforceRebase()       │        │ EnforceRebase()       │
│   isPush/isPR 판정     │        │   isPush/isPR 판정     │
│   fetch + behind 확인  │   →    │   workflow.RebaseOn   │
│   rebase 실행          │        │     Main() 호출       │
│   abort (에러 무시)     │        └───────────────────────┘
└───────────────────────┘
```

### 에러 처리 정책

| 파이프라인 | 단계 | 실패 시 |
|-----------|------|--------|
| Start | ValidateNewBranch | fatal — 브랜치 중복, 중단 |
| Start | IPC notify-start | fatal — DB 등록 실패, 중단 |
| Start | CreateAndPushBranch | fatal — git 실패, 중단 (DB 레코드 잔존, 재시도 가능) |
| Start | SyncImport | **non-fatal** — 경고 로그만. 착수 자체는 진행 |
| Merge | RebaseOnMain | fatal — 충돌 미해결, 머지 불가 |
| Merge | SyncImport | fatal — 동기화 실패, 머지 불가 |
| Merge | SyncExport | fatal — MD 갱신 실패, 머지 불가 |
| Merge | IPC notify-merge | fatal — FIXING 잔존 등, 머지 불가 |
| Merge | CheckoutMain | **non-fatal** — 경고만 (기존 동작 유지) |
| Drop | IPC notify-drop | fatal — FIXING 잔존 등, 드롭 불가 |
| Drop | CheckoutMain | **non-fatal** — 경고만 |

### SyncImport가 Start에서 non-fatal인 이유

착수 시점에 BACKLOG.md가 없거나 파싱 실패해도 작업 자체는 진행 가능. import는 "있으면 좋은" 동기화이지 착수의 전제조건이 아니다.

### 테스트 전략

- `git.go`: E2E 테스트 (실제 git repo 필요). 기존 `e2e/git_test.go` 패턴 활용
- `sync.go`: 단위 테스트 — 임시 디렉토리에 BACKLOG.md 생성 → SyncImport/Export 검증
- `pipeline.go`: E2E 테스트 — mock IPCFunc으로 파이프라인 시퀀스 검증
- `RebaseOnMain` abort 경로: `rebase.go`의 기존 테스트(`rebase_test.go`) 확장

### 파일 변경 목록

| 파일 | 변경 |
|------|------|
| `internal/workflow/git.go` | **신규** — git 유틸 4개 함수 |
| `internal/workflow/sync.go` | **신규** — SyncImport, SyncExport |
| `internal/workflow/pipeline.go` | **신규** — StartPipeline, MergePipeline, DropPipeline, IPCFunc |
| `internal/cli/handoff_cmd.go` | **수정** — doNotifyStart, notifyMergeCmd, notifyDropCmd를 파이프라인 호출로 교체 |
| `internal/modules/hook/rebase.go` | **수정** — rebase 코어를 workflow.RebaseOnMain() 위임, abort 에러 핸들링 |
| `e2e/workflow_test.go` | **신규** — 파이프라인 E2E 테스트 |

### 미변경 파일

- `internal/modules/handoff/manager.go` — 순수 DB 레이어 유지
- `internal/modules/backlog/export.go` — SafeExport() 그대로 사용
- `internal/modules/backlog/import.go` — ImportItems() 그대로 사용
- `internal/cli/handoff_cmd.go`의 design/plan 커맨드 — 변경 없음
