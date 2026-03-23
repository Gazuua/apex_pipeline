# 워크플로우 공유 레이어 + BACKLOG-157 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** apex-agent handoff CLI의 인라인 로직을 `internal/workflow` 패키지로 추출하여 CLI/HTTP 공유 가능하게 하고, 백로그 import/export 자동화 + BACKLOG-157 rebase abort 에러 핸들링을 구현한다.

**Architecture:** 신규 `internal/workflow` 패키지에 git 유틸(git.go), 백로그 동기화(sync.go), 파이프라인 조합(pipeline.go)을 배치. CLI는 파이프라인 함수를 호출하도록 리팩터링. IPC 호출은 `IPCFunc` 콜백으로 추상화하여 미래 HTTP 대시보드에서도 동일 파이프라인 사용 가능.

**Tech Stack:** Go 1.23, cobra CLI, SQLite (기존 backlog.Manager)

**Spec:** `docs/apex_tools/plans/20260323_154544_workflow_shared_layer_spec.md`

---

## 파일 구조

| 파일 | 역할 | 변경 |
|------|------|------|
| `internal/workflow/git.go` | git 조작 유틸 4개 함수 | **신규** |
| `internal/workflow/sync.go` | SyncImport, SyncExport | **신규** |
| `internal/workflow/pipeline.go` | IPCFunc, StartPipeline, MergePipeline, DropPipeline | **신규** |
| `internal/workflow/git_test.go` | git 유틸 단위 테스트 | **신규** |
| `internal/workflow/sync_test.go` | sync 단위 테스트 | **신규** |
| `internal/modules/hook/rebase.go` | EnforceRebase → workflow.RebaseOnMain 위임 | **수정** |
| `internal/cli/handoff_cmd.go` | 파이프라인 호출로 교체 | **수정** |
| `e2e/workflow_test.go` | 파이프라인 E2E 테스트 | **신규** |

---

### Task 1: git.go — git 조작 유틸 (테스트 → 구현)

**Files:**
- Create: `internal/workflow/git.go`
- Create: `internal/workflow/git_test.go`

- [ ] **Step 1: 패키지 스캐폴드 + ValidateNewBranch 테스트 작성**

`internal/workflow/git_test.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func runGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir}, args...)...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func initTestRepo(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	runGit(t, dir, "init", "-b", "main")
	runGit(t, dir, "config", "user.email", "test@test.com")
	runGit(t, dir, "config", "user.name", "Test")
	os.WriteFile(filepath.Join(dir, "README.md"), []byte("# test\n"), 0o644)
	runGit(t, dir, "add", ".")
	runGit(t, dir, "commit", "-m", "initial")
	return dir
}

func TestValidateNewBranch_OK(t *testing.T) {
	dir := initTestRepo(t)
	if err := ValidateNewBranch(dir, "feature/new"); err != nil {
		t.Errorf("expected no error for new branch, got: %v", err)
	}
}

func TestValidateNewBranch_LocalExists(t *testing.T) {
	dir := initTestRepo(t)
	runGit(t, dir, "checkout", "-b", "feature/exists")
	runGit(t, dir, "checkout", "main")
	err := ValidateNewBranch(dir, "feature/exists")
	if err == nil {
		t.Fatal("expected error for existing local branch")
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestValidateNewBranch -v`
Expected: FAIL — `ValidateNewBranch` 미정의

- [ ] **Step 3: git.go 구현 — ValidateNewBranch**

`internal/workflow/git.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"fmt"
	"os/exec"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("workflow")

// ValidateNewBranch checks that branchName does not exist locally or remotely.
func ValidateNewBranch(projectRoot, branchName string) error {
	// Local check
	if err := exec.Command("git", "-C", projectRoot,
		"rev-parse", "--verify", "refs/heads/"+branchName).Run(); err == nil {
		return fmt.Errorf("로컬 git 브랜치 '%s'가 이미 존재합니다", branchName)
	}
	// Remote check
	out, _ := exec.Command("git", "-C", projectRoot,
		"ls-remote", "--heads", "origin", branchName).Output()
	if len(strings.TrimSpace(string(out))) > 0 {
		return fmt.Errorf("리모트 git 브랜치 'origin/%s'가 이미 존재합니다", branchName)
	}
	return nil
}
```

- [ ] **Step 4: ValidateNewBranch 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestValidateNewBranch -v`
Expected: PASS

- [ ] **Step 5: CreateAndPushBranch 테스트 + 구현**

`git_test.go`에 추가:
```go
func TestCreateAndPushBranch(t *testing.T) {
	dir := initTestRepo(t)
	if err := CreateAndPushBranch(dir, "feature/new"); err != nil {
		t.Fatalf("CreateAndPushBranch: %v", err)
	}
	branch := runGit(t, dir, "branch", "--show-current")
	if branch != "feature/new" {
		t.Errorf("expected feature/new, got %s", branch)
	}
}
```

`git.go`에 추가:
```go
// CreateAndPushBranch creates a local branch and pushes with upstream tracking.
func CreateAndPushBranch(projectRoot, branchName string) error {
	if out, err := exec.Command("git", "-C", projectRoot,
		"checkout", "-b", branchName).CombinedOutput(); err != nil {
		return fmt.Errorf("git checkout -b 실패: %w\n%s", err, out)
	}
	if out, err := exec.Command("git", "-C", projectRoot,
		"push", "-u", "origin", branchName).CombinedOutput(); err != nil {
		ml.Warn("git push -u 실패 (재시도 필요)", "branch", branchName, "err", err)
		// push 실패는 경고만 — 브랜치 생성은 이미 완료
	}
	return nil
}
```

- [ ] **Step 6: CreateAndPushBranch 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestCreateAndPushBranch -v`
Expected: PASS (push는 origin 없어서 경고만 출력)

- [ ] **Step 7: RebaseOnMain 테스트 + 구현 (BACKLOG-157 포함)**

`git_test.go`에 추가:
```go
func initRepoWithOrigin(t *testing.T) (repoDir, bareDir string) {
	t.Helper()
	repoDir = initTestRepo(t)
	bareDir = t.TempDir()
	cmd := exec.Command("git", "clone", "--bare", repoDir, bareDir)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git clone --bare: %v\n%s", err, out)
	}
	runGit(t, repoDir, "remote", "add", "origin", bareDir)
	runGit(t, repoDir, "fetch", "origin")
	return
}

func TestRebaseOnMain_AlreadyUpToDate(t *testing.T) {
	repoDir, _ := initRepoWithOrigin(t)
	runGit(t, repoDir, "checkout", "-b", "feature/test")
	msg, err := RebaseOnMain(repoDir)
	if err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	if msg != "" {
		t.Errorf("expected empty msg for up-to-date, got: %q", msg)
	}
}

func TestRebaseOnMain_CleanRebase(t *testing.T) {
	repoDir, bareDir := initRepoWithOrigin(t)
	// feature 브랜치에서 별도 파일 커밋
	runGit(t, repoDir, "checkout", "-b", "feature/clean")
	os.WriteFile(filepath.Join(repoDir, "feature.txt"), []byte("feature\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "feature commit")
	// main에 별도 파일 커밋 (충돌 없음)
	runGit(t, repoDir, "checkout", "main")
	os.WriteFile(filepath.Join(repoDir, "main.txt"), []byte("main\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "main commit")
	runGit(t, repoDir, "push", bareDir, "main")
	// feature로 돌아와서 rebase
	runGit(t, repoDir, "checkout", "feature/clean")
	msg, err := RebaseOnMain(repoDir)
	if err != nil {
		t.Fatalf("expected clean rebase, got error: %v", err)
	}
	if !strings.Contains(msg, "rebase 완료") {
		t.Errorf("expected 'rebase 완료' in msg, got: %q", msg)
	}
}

func TestRebaseOnMain_ConflictAborts(t *testing.T) {
	repoDir, bareDir := initRepoWithOrigin(t)
	// 같은 파일 같은 줄 수정 → 충돌
	runGit(t, repoDir, "checkout", "-b", "feature/conflict")
	os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("feature version\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "feature change")
	runGit(t, repoDir, "checkout", "main")
	os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("main version\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "main change")
	runGit(t, repoDir, "push", bareDir, "main")
	runGit(t, repoDir, "checkout", "feature/conflict")
	_, err := RebaseOnMain(repoDir)
	if err == nil {
		t.Fatal("expected error on conflict")
	}
	if !strings.Contains(err.Error(), "충돌") {
		t.Errorf("expected '충돌' in error, got: %q", err.Error())
	}
}
```

`git.go`에 추가:
```go
// RebaseOnMain fetches origin/main and rebases the current branch.
// On conflict: aborts rebase (with error logging for BACKLOG-157) and returns error.
// Returns (message, error). Empty message if already up-to-date.
func RebaseOnMain(projectRoot string) (string, error) {
	// Fetch
	if err := exec.Command("git", "-C", projectRoot,
		"fetch", "origin", "main", "--quiet").Run(); err != nil {
		ml.Warn("git fetch origin main 실패, stale origin/main 사용", "err", err)
	}

	// Check behind count
	out, err := exec.Command("git", "-C", projectRoot,
		"rev-list", "--count", "HEAD..origin/main").Output()
	if err != nil {
		return "", nil // can't determine → assume OK
	}
	behind := strings.TrimSpace(string(out))
	if behind == "0" || behind == "" {
		return "", nil
	}

	// Rebase
	rebaseCmd := exec.Command("git", "-C", projectRoot, "rebase", "origin/main", "--quiet")
	if err := rebaseCmd.Run(); err != nil {
		// Abort — with proper error handling (BACKLOG-157)
		abortCmd := exec.Command("git", "-C", projectRoot, "rebase", "--abort")
		if abortErr := abortCmd.Run(); abortErr != nil {
			ml.Warn("rebase --abort 실패 — 수동 복구 필요",
				"rebase_err", err, "abort_err", abortErr)
			return "", fmt.Errorf("차단: rebase 충돌 + abort 실패. 수동 복구 필요:\n"+
				"  git rebase --abort  (또는 git rebase --continue)\n"+
				"  rebase 에러: %v\n  abort 에러: %v", err, abortErr)
		}
		return "", fmt.Errorf("차단: origin/main rebase 중 충돌 발생.\n"+
			"  수동으로 해결 후 다시 시도하세요:\n"+
			"  git fetch origin main && git rebase origin/main")
	}

	return fmt.Sprintf("[workflow] origin/main 기준 rebase 완료 (%s개 커밋 반영)", behind), nil
}
```

- [ ] **Step 8: RebaseOnMain 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestRebaseOnMain -v`
Expected: PASS (3개 서브테스트 모두)

- [ ] **Step 9: CheckoutMain 테스트 + 구현**

`git_test.go`에 추가:
```go
func TestCheckoutMain(t *testing.T) {
	dir := initTestRepo(t)
	runGit(t, dir, "checkout", "-b", "feature/test")
	if err := CheckoutMain(dir); err != nil {
		t.Fatalf("CheckoutMain: %v", err)
	}
	branch := runGit(t, dir, "branch", "--show-current")
	if branch != "main" {
		t.Errorf("expected main, got %s", branch)
	}
}
```

`git.go`에 추가:
```go
// CheckoutMain switches to main and pulls latest.
func CheckoutMain(projectRoot string) error {
	if out, err := exec.Command("git", "-C", projectRoot,
		"checkout", "main").CombinedOutput(); err != nil {
		return fmt.Errorf("checkout main: %w\n%s", err, out)
	}
	if out, err := exec.Command("git", "-C", projectRoot,
		"pull", "origin", "main").CombinedOutput(); err != nil {
		ml.Warn("git pull origin main 실패", "err", err)
		// pull 실패는 경고만
	}
	return nil
}
```

- [ ] **Step 10: 전체 git 테스트 통과 확인 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -v`
Expected: 모든 테스트 PASS

```bash
git add apex_tools/apex-agent/internal/workflow/
git commit -m "feat(tools): workflow/git.go — git 유틸 4개 함수 + BACKLOG-157 abort 에러 핸들링"
git push
```

---

### Task 2: sync.go — 백로그 동기화 (테스트 → 구현)

**Files:**
- Create: `internal/workflow/sync.go`
- Create: `internal/workflow/sync_test.go`
- Reference: `internal/modules/backlog/import.go`, `internal/modules/backlog/export.go`
- Reference: `internal/cli/backlog_cmd.go:197` (`openBacklogStore` 패턴)

- [ ] **Step 1: SyncImport 테스트 작성**

`internal/workflow/sync_test.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupSyncTest(t *testing.T) (string, *backlog.Manager, func()) {
	t.Helper()
	dir := t.TempDir()
	// docs/ 디렉토리 생성
	os.MkdirAll(filepath.Join(dir, "docs"), 0o755)

	dbPath := filepath.Join(dir, "test.db")
	s, err := store.Open(dbPath)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	mig := store.NewMigrator(s)
	mod := backlog.New(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		s.Close()
		t.Fatalf("migrate: %v", err)
	}
	return dir, mod.Manager(), func() { s.Close() }
}

func TestSyncImport_NewItems(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	md := `# BACKLOG

다음 발번: 3

---

## NOW

### #1. 테스트 이슈
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: 테스트 설명

---

## IN VIEW

---

## DEFERRED
`
	os.WriteFile(filepath.Join(dir, "docs", "BACKLOG.md"), []byte(md), 0o644)

	n, err := SyncImport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport: %v", err)
	}
	if n == 0 {
		t.Error("expected at least 1 imported item")
	}

	// DB에서 확인
	exists, status, _ := mgr.Check(1)
	if !exists {
		t.Fatal("item #1 not found in DB")
	}
	if status != "OPEN" {
		t.Errorf("expected OPEN, got %s", status)
	}
}

func TestSyncImport_NoFile(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	// BACKLOG.md 없음 — 에러 없이 0 반환
	n, err := SyncImport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport with no file: %v", err)
	}
	if n != 0 {
		t.Errorf("expected 0, got %d", n)
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestSyncImport -v`
Expected: FAIL — `SyncImport` 미정의

- [ ] **Step 3: sync.go 구현 — SyncImport**

`internal/workflow/sync.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"os"
	"path/filepath"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
)

// SyncImport reads BACKLOG.md + BACKLOG_HISTORY.md and imports into DB.
// Idempotent — existing items keep their status, only metadata is updated.
// Returns the number of newly imported items. Missing files are not an error.
func SyncImport(projectRoot string, mgr *backlog.Manager) (int, error) {
	total := 0

	backlogPath := filepath.Join(projectRoot, "docs", "BACKLOG.md")
	if data, err := os.ReadFile(backlogPath); err == nil {
		items, parseErr := backlog.ParseBacklogMD(string(data))
		if parseErr != nil {
			return total, parseErr
		}
		n, importErr := mgr.ImportItems(items)
		if importErr != nil {
			return total, importErr
		}
		total += n
	}

	historyPath := filepath.Join(projectRoot, "docs", "BACKLOG_HISTORY.md")
	if data, err := os.ReadFile(historyPath); err == nil {
		items, parseErr := backlog.ParseBacklogHistoryMD(string(data))
		if parseErr != nil {
			return total, parseErr
		}
		n, importErr := mgr.ImportItems(items)
		if importErr != nil {
			return total, importErr
		}
		total += n
	}

	if total > 0 {
		ml.Info("backlog import 완료", "items", total)
	}
	return total, nil
}
```

- [ ] **Step 4: SyncImport 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestSyncImport -v`
Expected: PASS

- [ ] **Step 5: SyncExport 테스트 + 구현**

`sync_test.go`에 추가:
```go
func TestSyncExport_WritesFile(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	// DB에 직접 항목 추가
	item := &backlog.BacklogItem{
		ID: 1, Title: "테스트", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명", Status: "OPEN",
	}
	if err := mgr.Add(item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	n, err := SyncExport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}
	_ = n

	// 파일 존재 확인
	outPath := filepath.Join(dir, "docs", "BACKLOG.md")
	data, err := os.ReadFile(outPath)
	if err != nil {
		t.Fatalf("read exported file: %v", err)
	}
	if len(data) == 0 {
		t.Error("exported file is empty")
	}
}
```

`sync.go`에 추가:
```go
// SyncExport runs SafeExport (import-first + export in single TX) and
// writes the result to docs/BACKLOG.md.
// Returns the number of items synced during import-first phase.
func SyncExport(projectRoot string, mgr *backlog.Manager) (int, error) {
	backlogPath := filepath.Join(projectRoot, "docs", "BACKLOG.md")
	historyPath := filepath.Join(projectRoot, "docs", "BACKLOG_HISTORY.md")

	backlogData, _ := os.ReadFile(backlogPath)
	historyData, _ := os.ReadFile(historyPath)

	content, imported, err := mgr.SafeExport(string(backlogData), string(historyData))
	if err != nil {
		return imported, err
	}

	if err := os.MkdirAll(filepath.Join(projectRoot, "docs"), 0o755); err != nil {
		return imported, err
	}
	if err := os.WriteFile(backlogPath, []byte(content), 0o644); err != nil {
		return imported, err
	}

	ml.Info("backlog export 완료", "imported", imported)
	return imported, nil
}
```

- [ ] **Step 6: 전체 sync 테스트 통과 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestSync -v`
Expected: PASS

```bash
git add apex_tools/apex-agent/internal/workflow/sync.go apex_tools/apex-agent/internal/workflow/sync_test.go
git commit -m "feat(tools): workflow/sync.go — SyncImport/SyncExport 백로그 동기화"
git push
```

---

### Task 3: pipeline.go — 파이프라인 조합 (테스트 → 구현)

**Files:**
- Create: `internal/workflow/pipeline.go`
- Create: `internal/workflow/pipeline_test.go`

- [ ] **Step 1: IPCFunc 타입 + MergePipeline 테스트 작성**

`internal/workflow/pipeline_test.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// mockIPC records calls and returns configurable results.
type mockIPC struct {
	calls  []string
	result map[string]any
	err    error
}

func (m *mockIPC) fn(action string, params map[string]any) (map[string]any, error) {
	m.calls = append(m.calls, action)
	return m.result, m.err
}

func setupPipelineTest(t *testing.T) (string, *backlog.Manager, func()) {
	t.Helper()
	dir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "docs"), 0o755)

	dbPath := filepath.Join(dir, "test.db")
	s, err := store.Open(dbPath)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	mig := store.NewMigrator(s)
	mod := backlog.New(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		s.Close()
		t.Fatalf("migrate: %v", err)
	}
	return dir, mod.Manager(), func() { s.Close() }
}

func TestDropPipeline(t *testing.T) {
	dir := initTestRepo(t)
	// feature 브랜치에서 시작
	runGit(t, dir, "checkout", "-b", "feature/drop-test")

	mock := &mockIPC{result: map[string]any{}}
	params := map[string]any{"branch": "test", "reason": "테스트"}

	err := DropPipeline(params, dir, mock.fn)
	if err != nil {
		t.Fatalf("DropPipeline: %v", err)
	}
	if len(mock.calls) != 1 || mock.calls[0] != "notify-drop" {
		t.Errorf("expected IPC call 'notify-drop', got: %v", mock.calls)
	}
}

func TestDropPipeline_IPCFails(t *testing.T) {
	dir := initTestRepo(t)
	mock := &mockIPC{err: fmt.Errorf("FIXING 백로그 잔존")}
	params := map[string]any{"branch": "test", "reason": "테스트"}

	err := DropPipeline(params, dir, mock.fn)
	if err == nil {
		t.Fatal("expected error when IPC fails")
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestDropPipeline -v`
Expected: FAIL — `DropPipeline` 미정의

- [ ] **Step 3: pipeline.go 구현**

`internal/workflow/pipeline.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
)

// IPCFunc is the abstraction for notify IPC calls.
// CLI: sendHandoffRequest() wrapper.
// HTTP: direct Manager method call wrapper.
type IPCFunc func(action string, params map[string]any) (map[string]any, error)

// StartPipeline orchestrates the full notify-start workflow:
//  1. ValidateNewBranch(branchName)
//  2. ipcFn("notify-start", params) → DB TX
//  3. CreateAndPushBranch(branchName)
//  4. SyncImport(projectRoot, mgr) — non-fatal
func StartPipeline(branchName string, params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) (float64, error) {

	// Phase 1: git 사전 검증
	if err := ValidateNewBranch(projectRoot, branchName); err != nil {
		return 0, err
	}

	// Phase 2: DB TX
	result, err := ipcFn("notify-start", params)
	if err != nil {
		return 0, fmt.Errorf("notify-start 실패: %w", err)
	}
	notifID, _ := result["notification_id"].(float64)

	// Phase 3: git 브랜치 생성
	if err := CreateAndPushBranch(projectRoot, branchName); err != nil {
		return notifID, fmt.Errorf("브랜치 생성 실패: %w", err)
	}

	// Phase 4: backlog import (non-fatal)
	if mgr != nil {
		if n, syncErr := SyncImport(projectRoot, mgr); syncErr != nil {
			ml.Warn("착수 시 backlog import 실패 (작업 진행에 영향 없음)", "err", syncErr)
		} else if n > 0 {
			ml.Info("착수 시 backlog import 완료", "items", n)
		}
	}

	return notifID, nil
}

// MergePipeline orchestrates the full notify-merge workflow:
//  1. RebaseOnMain(projectRoot)
//  2. SyncImport(projectRoot, mgr) — fatal
//  3. SyncExport(projectRoot, mgr) — fatal
//  4. ipcFn("notify-merge", params) — fatal
//  5. CheckoutMain(projectRoot) — non-fatal
func MergePipeline(params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) error {

	// Phase 1: rebase
	if msg, err := RebaseOnMain(projectRoot); err != nil {
		return err
	} else if msg != "" {
		fmt.Println(msg)
	}

	// Phase 2: import (rebase 후 최신 MD → DB)
	if mgr != nil {
		if _, err := SyncImport(projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog import 실패: %w", err)
		}
	}

	// Phase 3: export (DB → MD)
	if mgr != nil {
		if _, err := SyncExport(projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog export 실패: %w", err)
		}
	}

	// Phase 4: DB TX
	if _, err := ipcFn("notify-merge", params); err != nil {
		return fmt.Errorf("notify-merge 실패: %w", err)
	}

	// Phase 5: checkout main (non-fatal)
	if err := CheckoutMain(projectRoot); err != nil {
		ml.Warn("git checkout main 실패", "err", err)
	}

	return nil
}

// DropPipeline orchestrates the full notify-drop workflow:
//  1. ipcFn("notify-drop", params) → DB TX
//  2. CheckoutMain(projectRoot) — non-fatal
func DropPipeline(params map[string]any,
	projectRoot string, ipcFn IPCFunc) error {

	if _, err := ipcFn("notify-drop", params); err != nil {
		return fmt.Errorf("notify-drop 실패: %w", err)
	}

	if err := CheckoutMain(projectRoot); err != nil {
		ml.Warn("git checkout main 실패", "err", err)
	}

	return nil
}
```

- [ ] **Step 4: 전체 pipeline 테스트 통과 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -v`
Expected: PASS

```bash
git add apex_tools/apex-agent/internal/workflow/pipeline.go apex_tools/apex-agent/internal/workflow/pipeline_test.go
git commit -m "feat(tools): workflow/pipeline.go — Start/Merge/Drop 파이프라인 + IPCFunc 추상화"
git push
```

---

### Task 4: EnforceRebase 리팩터링

**Files:**
- Modify: `internal/modules/hook/rebase.go`
- Modify: `internal/modules/hook/rebase_test.go` (기존 테스트 유지 확인)

- [ ] **Step 1: rebase.go 리팩터링 — workflow.RebaseOnMain 위임**

`internal/modules/hook/rebase.go` 수정:
```go
// EnforceRebase checks if the current branch needs rebasing on origin/main.
// Only triggers on git push and gh pr create. Delegates actual rebase to
// workflow.RebaseOnMain().
func EnforceRebase(command, projectRoot string) (string, error) {
	// Only intercept git push and gh pr create
	if !isGitPush(command) && !isGHPRCreate(command) {
		return "", nil
	}

	// Get current branch
	branch, err := gitCurrentBranch(projectRoot)
	if err != nil {
		return "", nil
	}

	// Skip main/master/detached
	if branch == "main" || branch == "master" || branch == "HEAD" {
		return "", nil
	}

	// Delegate to workflow package
	return workflow.RebaseOnMain(projectRoot)
}
```

import에 `"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"` 추가.

기존의 fetch/behind/rebase/abort 로직 (L33~57) 전부 삭제 — `RebaseOnMain`으로 대체.

- [ ] **Step 2: 기존 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/modules/hook/... -v`
Expected: PASS (기존 단위 테스트 전부 통과)

Run: `cd apex_tools/apex-agent && go test ./e2e/... -run TestEnforceRebase -v`
Expected: PASS (E2E 테스트 통과)

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/hook/rebase.go
git commit -m "refactor(tools): EnforceRebase → workflow.RebaseOnMain 위임 + BACKLOG-157 해결"
git push
```

---

### Task 5: CLI 리팩터링 — 파이프라인 호출로 교체

**Files:**
- Modify: `internal/cli/handoff_cmd.go`

- [ ] **Step 1: doNotifyStart 리팩터링**

`handoff_cmd.go`의 `doNotifyStart()` (L112~159) 수정:

기존 인라인 로직 → `workflow.StartPipeline()` 호출로 교체.

```go
func doNotifyStart(branchName, summary string, backlogs []int, scopes string, skipDesign bool) error {
	branch := getBranchID()

	if backlogs == nil {
		backlogs = []int{}
	}
	params := map[string]any{
		"branch":      branch,
		"workspace":   branch,
		"branch_name": branchName,
		"summary":     summary,
		"backlog_ids": backlogs,
		"scopes":      scopes,
		"skip_design": skipDesign,
	}

	root, err := projectRoot()
	if err != nil {
		root = "." // fallback
	}

	// backlog manager (best-effort — import은 non-fatal)
	_, mgr, cleanup, mgrErr := openBacklogStore()
	if mgrErr != nil {
		mgr = nil // import 스킵
	} else {
		defer cleanup()
	}

	notifID, err := workflow.StartPipeline(branchName, params, root, mgr, ipcWrapper)
	if err != nil {
		return err
	}

	mode := "scopes=" + scopes
	if len(backlogs) == 0 {
		mode = "job mode"
	}
	fmt.Printf("[handoff] Tier 1 notification published: #%.0f (branch=%s, git=%s, %s)\n",
		notifID, branch, branchName, mode)
	return nil
}

// ipcWrapper adapts sendHandoffRequest to workflow.IPCFunc.
func ipcWrapper(action string, params map[string]any) (map[string]any, error) {
	return sendHandoffRequest(action, params)
}
```

- [ ] **Step 2: handoffNotifyMergeCmd 리팩터링**

`handoffNotifyMergeCmd()` (L229~261) 수정:

```go
func handoffNotifyMergeCmd() *cobra.Command {
	var summary string

	cmd := &cobra.Command{
		Use:   "merge",
		Short: "머지 완료 알림",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			params := map[string]any{
				"branch":    branch,
				"workspace": branch,
				"summary":   summary,
			}

			root, err := projectRoot()
			if err != nil {
				root = "."
			}

			_, mgr, cleanup, mgrErr := openBacklogStore()
			if mgrErr != nil {
				mgr = nil
			} else {
				defer cleanup()
			}

			if err := workflow.MergePipeline(params, root, mgr, ipcWrapper); err != nil {
				return err
			}
			fmt.Printf("[handoff] branch merged (branch=%s)\n", branch)
			return nil
		},
	}

	cmd.Flags().StringVar(&summary, "summary", "", "머지 요약 (필수)")
	_ = cmd.MarkFlagRequired("summary")
	return cmd
}
```

- [ ] **Step 3: handoffNotifyDropCmd 리팩터링**

`handoffNotifyDropCmd()` (L265~296) 수정:

```go
func handoffNotifyDropCmd() *cobra.Command {
	var reason string

	cmd := &cobra.Command{
		Use:   "drop",
		Short: "작업 중도 포기",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			params := map[string]any{
				"branch":    branch,
				"workspace": branch,
				"reason":    reason,
			}

			root, err := projectRoot()
			if err != nil {
				root = "."
			}

			if err := workflow.DropPipeline(params, root, ipcWrapper); err != nil {
				return err
			}
			fmt.Printf("[handoff] branch dropped (branch=%s, reason=%s)\n", branch, reason)
			return nil
		},
	}

	cmd.Flags().StringVar(&reason, "reason", "", "포기 사유 (필수)")
	_ = cmd.MarkFlagRequired("reason")
	return cmd
}
```

- [ ] **Step 4: 불필요 코드 제거**

`handoff_cmd.go`에서 제거:
- `gitCheckoutMain()` 함수 (L298~307) — `workflow.CheckoutMain`으로 대체
- 기존 import 중 불필요한 것 정리

- [ ] **Step 5: import 추가 + 컴파일 확인**

`handoff_cmd.go` import에 추가:
```go
"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"
```

`openBacklogStore` 접근: `backlog_cmd.go`에 정의되어 있으므로 같은 `cli` 패키지 내에서 접근 가능.

Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`
Expected: 컴파일 성공

- [ ] **Step 6: 기존 E2E 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./e2e/... -v -count=1`
Expected: 기존 handoff/git 테스트 전부 PASS

- [ ] **Step 7: 커밋**

```bash
git add apex_tools/apex-agent/internal/cli/handoff_cmd.go
git commit -m "refactor(tools): handoff CLI → workflow 파이프라인 호출로 교체 (import/export 자동화)"
git push
```

---

### Task 6: E2E 파이프라인 테스트

**Files:**
- Create: `e2e/workflow_test.go`

- [ ] **Step 1: MergePipeline E2E 테스트 작성**

`e2e/workflow_test.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"
)

func TestMergePipeline_SyncsBacklog(t *testing.T) {
	env := testenv.New(t)
	repoDir := setupRebaseScenario_Clean(t, env)

	// docs/ 디렉토리에 BACKLOG.md 생성
	docsDir := filepath.Join(repoDir, "docs")
	os.MkdirAll(docsDir, 0o755)
	md := "# BACKLOG\n\n다음 발번: 2\n\n---\n\n## NOW\n\n### #1. E2E 테스트 이슈\n- **등급**: MAJOR\n- **스코프**: TOOLS\n- **타입**: BUG\n- **설명**: E2E 설명\n\n---\n\n## IN VIEW\n\n---\n\n## DEFERRED\n"
	os.WriteFile(filepath.Join(docsDir, "BACKLOG.md"), []byte(md), 0o644)

	// DB setup
	dbPath := filepath.Join(env.Dir, "merge-test.db")
	s, err := store.Open(dbPath)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer s.Close()
	mig := store.NewMigrator(s)
	mod := backlog.New(s)
	mod.RegisterSchema(mig)
	mig.Migrate()
	mgr := mod.Manager()

	// mock IPC — merge 성공
	mockFn := func(action string, params map[string]any) (map[string]any, error) {
		return map[string]any{}, nil
	}

	params := map[string]any{"branch": "test", "workspace": "test", "summary": "test merge"}
	err = workflow.MergePipeline(params, repoDir, mgr, mockFn)
	if err != nil {
		t.Fatalf("MergePipeline: %v", err)
	}

	// BACKLOG.md가 export로 갱신되었는지 확인
	data, _ := os.ReadFile(filepath.Join(docsDir, "BACKLOG.md"))
	if !strings.Contains(string(data), "E2E 테스트 이슈") {
		t.Error("exported BACKLOG.md should contain the test item")
	}
}
```

- [ ] **Step 2: E2E 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./e2e/... -run TestMergePipeline -v`
Expected: PASS

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/e2e/workflow_test.go
git commit -m "test(tools): MergePipeline E2E 테스트 — rebase + import + export 파이프라인 검증"
git push
```

---

### Task 7: 전체 테스트 + 빌드 검증

- [ ] **Step 1: Go 전체 테스트**

Run: `cd apex_tools/apex-agent && go test ./... -count=1 -v`
Expected: ALL PASS

- [ ] **Step 2: C++ 빌드 (변경 없으므로 스킵 조건 평가)**

C++ 소스 변경 없음 → Go 백엔드만 빌드 확인으로 충분.

Run: `cd apex_tools/apex-agent && go build -o apex-agent.exe ./cmd/apex-agent`
Expected: 성공

- [ ] **Step 3: 바이너리 설치 + 데몬 재시작**

```bash
apex-agent daemon stop
cp apex-agent.exe "$LOCALAPPDATA/apex-agent/apex-agent.exe"
apex-agent daemon start
```

- [ ] **Step 4: 수동 스모크 테스트**

```bash
# handoff status 정상 작동
apex-agent handoff status

# cleanup dry-run 정상 작동
apex-agent cleanup
```
