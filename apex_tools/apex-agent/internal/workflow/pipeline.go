// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

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
func StartPipeline(ctx context.Context, branchName string, params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) error {

	// Phase 1: git 사전 검증
	if err := ValidateNewBranch(projectRoot, branchName); err != nil {
		return err
	}

	// Phase 2: DB TX
	if _, err := ipcFn("notify-start", params); err != nil {
		return fmt.Errorf("notify-start 실패: %w", err)
	}

	// Phase 3: git 브랜치 생성
	// NOTE: Phase 2(DB TX) 성공 후 여기서 실패하면 DB에는 등록되어 있지만
	// git 브랜치가 없는 불일치 상태가 된다. 이 경우 재시도 시 NotifyStart의
	// 기존 항목 정리 로직(branch replace)이 자동 복구한다.
	if err := CreateAndPushBranch(projectRoot, branchName); err != nil {
		return fmt.Errorf("브랜치 생성 실패: %w", err)
	}

	return nil
}

// MergeFullParams contains all dependencies for MergeFullPipeline.
type MergeFullParams struct {
	ProjectRoot   string
	Branch        string // workspace ID
	Workspace     string
	Summary       string
	ImportFn      func(projectRoot string)                   // backlog import (non-fatal)
	ExportFn      func(projectRoot string) error             // backlog export (DB → JSON file)
	FinalizeFn    func(ctx context.Context) error            // DB finalize (active → history MERGED)
	LockAcquireFn func(ctx context.Context) error            // queue merge acquire
	LockReleaseFn func(ctx context.Context) error            // queue merge release
}

// MergeFullPipeline orchestrates the complete merge workflow as a single atomic operation:
//
//	① merge lock acquire
//	② backlog import (non-fatal) + export + commit
//	③ git fetch + rebase origin/main
//	④ git push --force-with-lease
//	⑤ gh pr merge --squash --delete-branch --admin
//	⑥ handoff finalize (active → history MERGED)
//	⑦ checkout main + pull (best-effort)
//	⑧ merge lock release (defer — always runs)
//
// Errors:
//   - Steps ①~⑤: error + rollback (③ rebase --abort if needed) + lock release
//   - Step ⑥: error (exit 1) — merge completed, DB state inconsistent.
//   - Step ⑦: warning (exit 0) — merge + finalize complete.
func MergeFullPipeline(ctx context.Context, params MergeFullParams) error {
	root := params.ProjectRoot

	// ① merge lock acquire
	ml.Info("MergeFullPipeline: lock acquire", "branch", params.Branch)
	if err := params.LockAcquireFn(ctx); err != nil {
		return fmt.Errorf("merge lock 획득 실패: %w", err)
	}

	// ⑧ lock release (defer — 성공/실패 모두)
	defer func() {
		if err := params.LockReleaseFn(context.Background()); err != nil {
			ml.Warn("merge lock 해제 실패", "err", err)
		}
	}()

	// ② backlog import (non-fatal) + export + commit
	if params.ImportFn != nil {
		params.ImportFn(root)
	}
	if params.ExportFn != nil {
		if err := params.ExportFn(root); err != nil {
			return fmt.Errorf("backlog export 실패: %w", err)
		}
	}
	if err := autoCommitExport(root); err != nil {
		return fmt.Errorf("export 커밋 실패: %w", err)
	}

	// ③ rebase
	if msg, err := RebaseOnMain(root); err != nil {
		return err
	} else if msg != "" {
		fmt.Println(msg)
	}

	// ④ push --force-with-lease
	if err := pushForceWithLease(root); err != nil {
		return err
	}

	// ⑤ gh pr merge
	if err := ghPRMerge(root); err != nil {
		return err
	}

	// ⑥ finalize (DB) — point of no return: context.Background()로 데몬 shutdown과 무관하게 완료.
	// 실패해도 에러 반환하지 않음 — 재실행 시 ①~⑤가 다시 돌아 복구 불가하므로 경고만 출력.
	// 다음 notify start가 stale entry를 자동 정리함.
	if err := params.FinalizeFn(context.Background()); err != nil {
		fmt.Fprintf(os.Stderr, "[merge] 경고: 핸드오프 정리 실패 — %v\n", err)
		fmt.Fprintln(os.Stderr, "  가이드: 다음 작업 착수(notify start) 시 자동 정리됩니다")
		ml.Warn("FinalizeFn failed (merge completed, DB stale)", "branch", params.Branch, "err", err)
	}

	// ⑦ checkout main (best-effort)
	if err := CheckoutMain(root); err != nil {
		fmt.Fprintf(os.Stderr, "[merge] 경고: checkout main 실패 — %v\n", err)
		fmt.Fprintln(os.Stderr, "  가이드: git checkout main && git pull origin main 수동 실행")
		// exit 0 — 머지+정리 모두 완료
	}

	ml.Audit("MergeFullPipeline completed", "branch", params.Branch)
	return nil
}

// pushForceWithLease pushes the current branch with --force-with-lease.
func pushForceWithLease(projectRoot string) error {
	ml.Info("pushForceWithLease")
	out, err := exec.Command("git", "-C", projectRoot,
		"push", "--force-with-lease").CombinedOutput()
	if err != nil {
		return fmt.Errorf("git push --force-with-lease 실패: %w\n%s", err, out)
	}
	ml.Info("push --force-with-lease 완료")
	return nil
}

// ghPRMerge runs gh pr merge in the project root directory.
// gh CLI requires CWD to be inside the git repo (no -C flag support).
func ghPRMerge(projectRoot string) error {
	ml.Info("ghPRMerge")
	cmd := exec.Command("gh", "pr", "merge", "--squash", "--delete-branch", "--admin")
	cmd.Dir = projectRoot
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("gh pr merge 실패: %w\n%s", err, out)
	}
	ml.Info("gh pr merge 완료")
	return nil
}

// autoCommitExport stages and commits backlog export results.
// No-op if there are no changes to commit.
func autoCommitExport(projectRoot string) error {
	// Stage — JSON + 레거시 MD (삭제분 포함). 존재하는 파일만 add, 삭제된 파일은 git rm --cached.
	for _, f := range []string{"docs/BACKLOG.json", "docs/BACKLOG.md", "docs/BACKLOG_HISTORY.md"} {
		fullPath := filepath.Join(projectRoot, f)
		if _, statErr := os.Stat(fullPath); statErr == nil {
			// 파일 존재 → git add
			if out, err := exec.Command("git", "-C", projectRoot, "add", f).CombinedOutput(); err != nil {
				return fmt.Errorf("git add %s: %w\n%s", f, err, out)
			}
		} else {
			// 파일 없음 → git rm --cached (이미 인덱스에 없으면 무시)
			exec.Command("git", "-C", projectRoot, "rm", "--cached", "--ignore-unmatch", f).Run()
		}
	}

	// Check if anything staged
	if err := exec.Command("git", "-C", projectRoot,
		"diff", "--cached", "--quiet").Run(); err == nil {
		return nil // nothing to commit
	}

	// Commit (push는 MergeFullPipeline의 ④ pushForceWithLease가 일괄 처리)
	if out, err := exec.Command("git", "-C", projectRoot,
		"commit", "-m", "docs: backlog export (auto-sync)").CombinedOutput(); err != nil {
		return fmt.Errorf("git commit: %w\n%s", err, out)
	}

	ml.Info("backlog export 자동 커밋 완료")
	return nil
}

// DropPipeline orchestrates the full notify-drop workflow:
//  1. ipcFn("notify-drop", params) → DB TX
//  2. CheckoutMain(projectRoot) — non-fatal
func DropPipeline(ctx context.Context, params map[string]any,
	projectRoot string, ipcFn IPCFunc) error {

	if _, err := ipcFn("notify-drop", params); err != nil {
		return fmt.Errorf("notify-drop 실패: %w", err)
	}

	if err := CheckoutMain(projectRoot); err != nil {
		ml.Warn("git checkout main 실패", "err", err)
	}

	return nil
}
