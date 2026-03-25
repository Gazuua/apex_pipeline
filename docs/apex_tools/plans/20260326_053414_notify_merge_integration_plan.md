# notify merge 통합 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `handoff notify merge`를 머지의 유일한 진입점으로 통합 — 데몬 파이프라인에서 lock→export→rebase→push→gh pr merge→finalize→checkout을 원자적으로 수행하고, `gh pr merge` 직접 호출은 hook에서 전면 차단

**Architecture:** handoff 모듈에 `QueueOperator` 인터페이스를 주입하여 queue 모듈과 협업. 새로운 `MergeFullPipeline` 함수를 workflow 패키지에 추가하여 전체 파이프라인을 orchestration. 데몬 IPC 핸들러(`handleNotifyMerge`)가 project_root를 받아 파이프라인 실행.

**Tech Stack:** Go, cobra CLI, SQLite, exec.Command (git/gh)

**Spec:** `docs/apex_tools/plans/20260326_053414_notify_merge_integration_spec.md`

---

### Task 1: QueueOperator 인터페이스 추가 + handoff 모듈에 주입

handoff 모듈이 queue 모듈의 lock을 사용할 수 있도록 인터페이스를 정의한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/modules/handoff/manager.go:36-53` — QueueOperator 인터페이스 + Manager 필드 추가
- Modify: `apex_tools/apex-agent/internal/modules/handoff/module.go:20-22` — New()에 QueueOperator 파라미터 추가
- Modify: `apex_tools/apex-agent/internal/cli/daemon_cmd.go:91-98` — handoff.New()에 queueMod.Manager() 주입

- [ ] **Step 1: manager.go에 QueueOperator 인터페이스 정의**

```go
// QueueOperator is the interface handoff needs from queue for merge lock management.
type QueueOperator interface {
	Acquire(ctx context.Context, channel, branch string, pid int) error
	Release(ctx context.Context, channel string) error
}
```

Manager 구조체에 `queueManager QueueOperator` 필드 추가, NewManager에 파라미터 추가.

- [ ] **Step 2: module.go의 New()에 QueueOperator 파라미터 추가**

```go
func New(s *store.Store, bm BacklogOperator, qm QueueOperator) *Module {
	return &Module{manager: NewManager(s, bm, qm)}
}
```

- [ ] **Step 3: daemon_cmd.go에서 queueMod.Manager()를 handoff.New()에 전달**

```go
handoffMod := handoffmod.New(d.Store(), backlogMod.Manager(), queueMod.Manager())
```

주의: 등록 순서 변경 — queue 모듈을 handoff보다 먼저 생성해야 함 (등록 순서는 무관, 생성 순서만 중요).

- [ ] **Step 4: 컴파일 확인**

Run: `cd apex_tools/apex-agent && go build ./...`
Expected: 컴파일 성공

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/handoff/manager.go \
        apex_tools/apex-agent/internal/modules/handoff/module.go \
        apex_tools/apex-agent/internal/cli/daemon_cmd.go
git commit -m "refactor(tools): handoff 모듈에 QueueOperator 인터페이스 주입 (BACKLOG-234)"
```

---

### Task 2: MergeFullPipeline 구현 (workflow 패키지)

데몬에서 실행되는 전체 머지 파이프라인을 workflow 패키지에 추가한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/workflow/pipeline.go` — MergeFullPipeline 함수 추가

- [ ] **Step 1: MergeFullPipeline 함수 추가**

기존 `MergePipeline`을 대체하는 새 함수. 전체 8단계를 orchestration한다.

```go
// MergeFullPipeline orchestrates the complete merge workflow as a single atomic operation:
//  ① merge lock acquire
//  ② backlog export + commit
//  ③ git fetch + rebase origin/main
//  ④ git push --force-with-lease
//  ⑤ gh pr merge --squash --delete-branch --admin
//  ⑥ handoff finalize (active → history MERGED)
//  ⑦ checkout main + pull (best-effort)
//  ⑧ merge lock release (defer — always runs)
//
// Errors:
//  - Steps ①~⑤: error + rollback (③ rebase --abort if needed) + lock release
//  - Step ⑥: error (exit 1) — merge completed, DB state inconsistent. Guide: re-run notify merge
//  - Step ⑦: warning (exit 0) — merge + finalize complete. Guide: manual checkout
func MergeFullPipeline(ctx context.Context, params MergeFullParams) error {
	// ... implementation
}
```

`MergeFullParams` 구조체:
```go
type MergeFullParams struct {
	ProjectRoot    string
	Branch         string // workspace ID
	Workspace      string
	Summary        string
	ExportFn       func(projectRoot string) error             // backlog export (IPC 경유 또는 직접)
	ImportFn       func(projectRoot string)                   // backlog import (non-fatal)
	FinalizeFn     func(ctx context.Context) error            // DB finalize (NotifyMerge)
	LockAcquireFn  func(ctx context.Context) error            // queue merge acquire
	LockReleaseFn  func(ctx context.Context) error            // queue merge release
}
```

파이프라인 내부 로직:
1. `LockAcquireFn(ctx)` — 머지 lock 획득
2. `defer LockReleaseFn(ctx)` — 항상 lock 해제
3. `ImportFn(projectRoot)` — non-fatal sync import
4. `ExportFn(projectRoot)` → `autoCommitExport(projectRoot)` — export + commit
5. `RebaseOnMain(projectRoot)` — fetch + rebase (실패 시 abort)
6. `git push --force-with-lease` — exec.Command
7. `gh pr merge --squash --delete-branch --admin` — exec.Command
8. `FinalizeFn(ctx)` — DB finalize. 실패 시 에러 반환 + 가이드 출력
9. `CheckoutMain(projectRoot)` — 실패 시 경고 출력 + 가이드, 에러 반환 안 함

- [ ] **Step 2: pushForceWithLease 헬퍼 함수 추가**

```go
func pushForceWithLease(projectRoot string) error {
	out, err := exec.Command("git", "-C", projectRoot,
		"push", "--force-with-lease").CombinedOutput()
	if err != nil {
		return fmt.Errorf("git push --force-with-lease: %w\n%s", err, out)
	}
	ml.Info("push --force-with-lease 완료")
	return nil
}
```

- [ ] **Step 3: ghPRMerge 헬퍼 함수 추가**

```go
func ghPRMerge(projectRoot string) error {
	out, err := exec.Command("gh", "pr", "merge",
		"--squash", "--delete-branch", "--admin",
	).CombinedOutput()
	if err != nil {
		return fmt.Errorf("gh pr merge: %w\n%s", err, out)
	}
	ml.Info("gh pr merge 완료")
	return nil
}
```

주: gh CLI는 git repo의 CWD에서 실행해야 하므로 `exec.Command`에 `.Dir = projectRoot` 설정 필요. `-C` 플래그 미지원.

- [ ] **Step 4: 기존 MergePipeline에 Deprecated 주석 추가**

기존 함수는 아직 제거하지 않음 (HTTP 대시보드 등에서 참조 가능). Deprecated 주석만 추가.

- [ ] **Step 5: 컴파일 확인**

Run: `cd apex_tools/apex-agent && go build ./...`

- [ ] **Step 6: 커밋**

```bash
git add apex_tools/apex-agent/internal/workflow/pipeline.go
git commit -m "feat(tools): MergeFullPipeline 구현 — 머지 전체 파이프라인 원자적 수행 (BACKLOG-234)"
```

---

### Task 3: handleNotifyMerge IPC 핸들러 확장

데몬의 IPC 핸들러가 project_root를 받아 MergeFullPipeline을 호출하도록 확장한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/modules/handoff/module.go:392-407` — handleNotifyMerge 확장
- Modify: `apex_tools/apex-agent/internal/modules/handoff/module.go:392-396` — notifyMergeParams에 ProjectRoot 추가

- [ ] **Step 1: notifyMergeParams에 ProjectRoot 필드 추가**

```go
type notifyMergeParams struct {
	Branch      string `json:"branch"`
	Workspace   string `json:"workspace"`
	Summary     string `json:"summary"`
	ProjectRoot string `json:"project_root"`
}
```

- [ ] **Step 2: handleNotifyMerge에서 MergeFullPipeline 호출**

```go
func (m *Module) handleNotifyMerge(ctx context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyMergeParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}

	mergeParams := workflow.MergeFullParams{
		ProjectRoot: p.ProjectRoot,
		Branch:      p.Branch,
		Workspace:   p.Workspace,
		Summary:     p.Summary,
		ImportFn: func(root string) {
			// non-fatal sync import — 데몬 내부에서 직접 실행
			if m.manager.backlogManager != nil {
				workflow.SyncImport(ctx, root, /* backlog mgr from operator */)
			}
		},
		ExportFn: func(root string) error {
			// 데몬 내부에서 직접 export
			return workflow.SyncExportDirect(ctx, root, /* backlog mgr */)
		},
		FinalizeFn: func(fCtx context.Context) error {
			return m.manager.NotifyMerge(fCtx, p.Branch, p.Workspace, p.Summary)
		},
		LockAcquireFn: func(lCtx context.Context) error {
			return m.manager.queueManager.Acquire(lCtx, "merge", p.Branch, os.Getpid())
		},
		LockReleaseFn: func(lCtx context.Context) error {
			return m.manager.queueManager.Release(lCtx, "merge")
		},
	}

	if err := workflow.MergeFullPipeline(ctx, mergeParams); err != nil {
		return nil, err
	}
	return map[string]string{"status": "merged"}, nil
}
```

주: backlog import/export는 데몬이 직접 DB 접근 가능하므로 IPC 경유 불필요. BacklogOperator에 Export 메서드를 추가하거나 SyncExport를 직접 호출.

- [ ] **Step 3: backlog export를 위한 ExportToFile 메서드 확인/추가**

backlog 모듈의 Manager에 `ExportToFile(ctx, projectRoot)` 메서드가 필요. 기존 `backlog export` CLI가 IPC → `export` 액션을 호출하므로, 데몬 내부에서는 Manager.Export()를 직접 호출 + 파일 쓰기하면 됨.

- [ ] **Step 4: 컴파일 확인**

Run: `cd apex_tools/apex-agent && go build ./...`

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/handoff/module.go
git commit -m "feat(tools): handleNotifyMerge에서 MergeFullPipeline 호출 (BACKLOG-234)"
```

---

### Task 4: CLI 단순화 — handoffNotifyMergeCmd

CLI가 IPC로 project_root만 전달하도록 단순화한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/cli/handoff_cmd.go:258-298` — handoffNotifyMergeCmd 단순화

- [ ] **Step 1: handoffNotifyMergeCmd 리팩터링**

기존 CLI 로직(syncImport, syncExport, MergePipeline 호출)을 제거하고 IPC 요청만 보내도록 변경.

```go
func handoffNotifyMergeCmd() *cobra.Command {
	var summary string

	cmd := &cobra.Command{
		Use:   "merge",
		Short: "머지 완료 — 전체 파이프라인 실행 (lock→export→rebase→push→merge→finalize)",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			root, err := projectRoot()
			if err != nil {
				return fmt.Errorf("프로젝트 루트를 찾을 수 없습니다: %w", err)
			}
			params := map[string]any{
				"branch":       branch,
				"workspace":    branch,
				"summary":      summary,
				"project_root": root,
			}
			// Extended timeout: 머지 파이프라인은 lock 대기 + rebase + push + merge를 포함
			if _, err := sendHandoffRequestWithTimeout("notify-merge", params, 35*time.Minute); err != nil {
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

- [ ] **Step 2: sendHandoffRequestWithTimeout 헬퍼 추가**

기존 `sendHandoffRequest`에 timeout 파라미터를 추가한 변형. merge lock 대기(최대 30분)를 커버하기 위해 35분 timeout 필요.

- [ ] **Step 3: 컴파일 확인**

Run: `cd apex_tools/apex-agent && go build ./...`

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/internal/cli/handoff_cmd.go
git commit -m "refactor(tools): handoff notify merge CLI 단순화 — IPC 전달만 수행 (BACKLOG-234)"
```

---

### Task 5: validate-merge hook — gh pr merge 전면 차단

`gh pr merge` 직접 호출을 무조건 차단하도록 validate-merge hook을 변경한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/cli/hook_cmd.go:49-101` — hookValidateMergeCmd 변경

- [ ] **Step 1: gh pr merge 차단 로직 변경**

기존: merge lock 확인 + `--delete-branch` 강제
변경: 무조건 차단 + 안내 메시지

```go
// gh pr merge: 직접 호출 전면 차단
if containsShellCommand(command, "gh pr merge") {
	fmt.Fprintln(os.Stderr, "차단: gh pr merge 직접 호출 금지 — apex-agent handoff notify merge --summary \"...\"를 사용하세요")
	os.Exit(2)
}
```

merge lock 확인 로직(라인 76-95) 전체 제거.

- [ ] **Step 2: hookValidateMergeCmd의 Short 설명 갱신**

```go
Short: "PR 명령 검증 (gh pr merge 차단 + base main 강제)",
```

- [ ] **Step 3: 컴파일 확인**

Run: `cd apex_tools/apex-agent && go build ./...`

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/internal/cli/hook_cmd.go
git commit -m "feat(tools): validate-merge hook에서 gh pr merge 직접 호출 전면 차단 (BACKLOG-234)"
```

---

### Task 6: queue merge 서브커맨드 제거

에이전트가 직접 사용할 일 없는 `queue merge acquire/release/status`를 CLI에서 제거한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/cli/queue_cmd.go:29,319-363` — queueMergeCmd 제거

- [ ] **Step 1: queueMergeCmd() 함수 및 등록 제거**

`queue_cmd.go`에서:
- `cmd.AddCommand(queueMergeCmd())` 라인 제거 (라인 29)
- `queueMergeCmd()` 함수 전체 제거 (라인 321-363)

- [ ] **Step 2: 컴파일 확인**

Run: `cd apex_tools/apex-agent && go build ./...`

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/internal/cli/queue_cmd.go
git commit -m "refactor(tools): queue merge 서브커맨드 제거 — notify merge 내부로 이동 (BACKLOG-234)"
```

---

### Task 7: 테스트

기존 테스트 수정 + 새 E2E 테스트 추가.

**Files:**
- Modify: `apex_tools/apex-agent/internal/modules/handoff/manager_test.go` — QueueOperator mock 추가
- Modify: `apex_tools/apex-agent/e2e/handoff_test.go` — MergeFullPipeline E2E 테스트

- [ ] **Step 1: manager_test.go에 MockQueueOperator 추가**

```go
type mockQueueOperator struct {
	acquireCalled bool
	releaseCalled bool
	acquireErr    error
	releaseErr    error
}

func (m *mockQueueOperator) Acquire(ctx context.Context, channel, branch string, pid int) error {
	m.acquireCalled = true
	return m.acquireErr
}

func (m *mockQueueOperator) Release(ctx context.Context, channel string) error {
	m.releaseCalled = true
	return m.releaseErr
}
```

기존 NewManager 호출부에 mock 주입.

- [ ] **Step 2: 기존 단위 테스트 수정 — NewManager 시그니처 변경 반영**

모든 `NewManager(store, backlogMgr)` → `NewManager(store, backlogMgr, queueMgr)` 호출 갱신.

- [ ] **Step 3: 전체 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: ALL PASS

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/
git commit -m "test(tools): QueueOperator mock 추가 + 기존 테스트 시그니처 갱신 (BACKLOG-234)"
```

---

### Task 8: 기존 MergePipeline 참조 정리

MergeFullPipeline으로 전환 완료 후 기존 MergePipeline 및 관련 dead code를 정리한다.

**Files:**
- Modify: `apex_tools/apex-agent/internal/workflow/pipeline.go` — 기존 MergePipeline 제거
- Modify: `apex_tools/apex-agent/internal/cli/handoff_cmd.go` — syncImportViaIPC, syncExportViaIPC 제거 (더 이상 merge에서 미사용)

- [ ] **Step 1: MergePipeline 함수 제거 (pipeline.go)**

기존 MergePipeline은 더 이상 호출되지 않음. 제거.

- [ ] **Step 2: merge 전용 IPC 헬퍼 정리 (handoff_cmd.go)**

`syncImportViaIPC`, `syncExportViaIPC`가 merge 외에서도 사용되는지 확인.
- `syncImportViaIPC`: `doNotifyStart`에서 사용 → **유지**
- `syncExportViaIPC`: merge에서만 사용 → **제거**

- [ ] **Step 3: 컴파일 + 테스트 확인**

Run: `cd apex_tools/apex-agent && go build ./... && go test ./... -count=1`

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/
git commit -m "refactor(tools): 기존 MergePipeline + syncExportViaIPC 제거 (BACKLOG-234)"
```

---

### Task 9: 문서 갱신

CLAUDE.md, apex-agent CLAUDE.md 갱신.

**Files:**
- Modify: `CLAUDE.md` — § 머지 절차, § 큐 CLI
- Modify: `apex_tools/apex-agent/CLAUDE.md` — § 핸드오프 CLI, § 큐 CLI, § Hook 게이트

- [ ] **Step 1: CLAUDE.md (루트) — § 머지 절차 갱신**

`## 전역 규칙 > ### Git / 브랜치` 섹션에서:
- 6단계 수동 머지 절차 → `handoff notify merge --summary "..."` 원커맨드로 교체
- `queue merge acquire/release` 언급 제거
- `gh pr merge` 직접 호출 → hook 차단됨 명시

- [ ] **Step 2: apex_tools/apex-agent/CLAUDE.md — § 핸드오프 CLI 갱신**

`notify merge`의 내부 파이프라인 8단계 명시.

- [ ] **Step 3: apex_tools/apex-agent/CLAUDE.md — § 큐 CLI 갱신**

`queue merge acquire/release/status` 제거. `queue build`, `queue benchmark`, 범용 `queue acquire/release/status <channel>` 유지.

- [ ] **Step 4: apex_tools/apex-agent/CLAUDE.md — § Hook 게이트 갱신**

`validate-merge` 역할 변경: "merge lock + --delete-branch" → "gh pr merge 전면 차단 + --base main"

- [ ] **Step 5: 커밋**

```bash
git add CLAUDE.md apex_tools/apex-agent/CLAUDE.md
git commit -m "docs(tools): notify merge 통합 반영 — 머지 절차/CLI/hook 가이드 갱신 (BACKLOG-234)"
```
