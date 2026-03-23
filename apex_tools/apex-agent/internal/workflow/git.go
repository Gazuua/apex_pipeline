// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"fmt"
	"os/exec"
	"strconv"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("workflow")

// ValidateNewBranch checks that branchName does not exist locally or remotely.
func ValidateNewBranch(projectRoot, branchName string) error {
	if err := exec.Command("git", "-C", projectRoot,
		"rev-parse", "--verify", "refs/heads/"+branchName).Run(); err == nil {
		return fmt.Errorf("로컬 git 브랜치 '%s'가 이미 존재합니다", branchName)
	}
	out, _ := exec.Command("git", "-C", projectRoot,
		"ls-remote", "--heads", "origin", branchName).Output()
	if len(strings.TrimSpace(string(out))) > 0 {
		return fmt.Errorf("리모트 git 브랜치 'origin/%s'가 이미 존재합니다", branchName)
	}
	return nil
}

// CreateAndPushBranch creates a local branch and pushes with upstream tracking.
func CreateAndPushBranch(projectRoot, branchName string) error {
	if out, err := exec.Command("git", "-C", projectRoot,
		"checkout", "-b", branchName).CombinedOutput(); err != nil {
		return fmt.Errorf("git checkout -b 실패: %w\n%s", err, out)
	}
	if out, err := exec.Command("git", "-C", projectRoot,
		"push", "-u", "origin", branchName).CombinedOutput(); err != nil {
		ml.Warn("git push -u 실패 (재시도 필요)", "branch", branchName, "err", err, "output", string(out))
	}
	return nil
}

// RebaseOnMain fetches origin/main and rebases the current branch.
// On conflict: aborts rebase (with error logging — BACKLOG-157) and returns error.
// Returns (message, error). Empty message if already up-to-date.
func RebaseOnMain(projectRoot string) (string, error) {
	if err := exec.Command("git", "-C", projectRoot,
		"fetch", "origin", "main", "--quiet").Run(); err != nil {
		ml.Warn("git fetch origin main 실패, stale origin/main 사용", "err", err)
	}

	out, err := exec.Command("git", "-C", projectRoot,
		"rev-list", "--count", "HEAD..origin/main").Output()
	if err != nil {
		ml.Warn("git rev-list 실패 — rebase 필요 여부 판단 불가, 스킵", "err", err)
		return "", nil
	}
	behind, _ := strconv.Atoi(strings.TrimSpace(string(out)))
	if behind == 0 {
		return "", nil
	}

	rebaseCmd := exec.Command("git", "-C", projectRoot, "rebase", "origin/main", "--quiet")
	if err := rebaseCmd.Run(); err != nil {
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

	return fmt.Sprintf("[workflow] origin/main 기준 rebase 완료 (%d개 커밋 반영)", behind), nil
}

// CheckoutMain switches to main and pulls latest.
func CheckoutMain(projectRoot string) error {
	if out, err := exec.Command("git", "-C", projectRoot,
		"checkout", "main").CombinedOutput(); err != nil {
		return fmt.Errorf("checkout main: %w\n%s", err, out)
	}
	if out, err := exec.Command("git", "-C", projectRoot,
		"pull", "origin", "main").CombinedOutput(); err != nil {
		ml.Warn("git pull origin main 실패", "err", err, "output", string(out))
	}
	return nil
}
