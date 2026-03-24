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
//  4. SyncImport(projectRoot, mgr) — non-fatal
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

	// Phase 4: backlog import (non-fatal)
	if mgr != nil {
		if n, syncErr := SyncImport(ctx, projectRoot, mgr); syncErr != nil {
			ml.Warn("착수 시 backlog import 실패 (작업 진행에 영향 없음)", "err", syncErr)
		} else if n > 0 {
			ml.Info("착수 시 backlog import 완료", "items", n)
		}
	}

	return nil
}

// MergePipeline orchestrates the full merge workflow:
//  1. RebaseOnMain(projectRoot)
//  2. SyncImport(projectRoot, mgr) — rebase 후 최신 MD → DB
//  3. SyncExport(projectRoot, mgr) — DB → MD + HISTORY
//  4. autoCommitExport — export 결과 커밋+푸시
//  5. CheckoutMain — main 브랜치 전환
//  6. ipcFn("notify-merge") — 마지막: active에서 삭제, history로 이관
func MergePipeline(ctx context.Context, params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) error {

	// Phase 1: rebase
	if msg, err := RebaseOnMain(projectRoot); err != nil {
		return err
	} else if msg != "" {
		fmt.Println(msg)
	}

	// Phase 2: import (rebase 후 최신 MD → DB)
	if mgr != nil {
		if _, err := SyncImport(ctx, projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog import 실패: %w", err)
		}
	}

	// Phase 3: export (DB → MD + HISTORY)
	if mgr != nil {
		if _, err := SyncExport(ctx, projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog export 실패: %w", err)
		}
	}

	// Phase 4: auto-commit + push (export 결과)
	if err := autoCommitExport(projectRoot); err != nil {
		return fmt.Errorf("export 결과 커밋 실패: %w", err)
	}

	// Phase 5: checkout main
	if err := CheckoutMain(projectRoot); err != nil {
		return fmt.Errorf("checkout main 실패: %w", err)
	}

	// Phase 6: IPC notify-merge (마지막 — active에서 삭제)
	if _, err := ipcFn("notify-merge", params); err != nil {
		return fmt.Errorf("notify-merge 실패: %w", err)
	}

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

	// Commit
	if out, err := exec.Command("git", "-C", projectRoot,
		"commit", "-m", "docs: backlog export (auto-sync)").CombinedOutput(); err != nil {
		return fmt.Errorf("git commit: %w\n%s", err, out)
	}

	// Push (best-effort — 에이전트가 push --force-with-lease를 별도 실행할 수 있음)
	if out, err := exec.Command("git", "-C", projectRoot,
		"push").CombinedOutput(); err != nil {
		ml.Warn("autoCommitExport push 실패 (수동 push 필요)", "err", err, "output", string(out))
	}

	ml.Info("backlog export 자동 커밋+푸시 완료")
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
