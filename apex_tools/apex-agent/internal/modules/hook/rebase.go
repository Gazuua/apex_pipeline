// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"fmt"
	"os/exec"
	"strconv"
	"strings"
)

// EnforceRebase checks if the current branch needs rebasing on origin/main.
// If behind, attempts auto-rebase. Returns nil if OK, error if blocked (conflict).
// projectRoot is the git working directory.
func EnforceRebase(command, projectRoot string) (string, error) {
	// Only intercept git push and gh pr create
	if !isGitPush(command) && !isGHPRCreate(command) {
		return "", nil
	}

	// Get current branch
	branch, err := gitCurrentBranch(projectRoot)
	if err != nil {
		return "", nil // can't determine branch → allow
	}

	// Skip main/master/detached
	if branch == "main" || branch == "master" || branch == "HEAD" {
		return "", nil
	}

	// Fetch origin main
	exec.Command("git", "-C", projectRoot, "fetch", "origin", "main", "--quiet").Run() //nolint:errcheck

	// Check how far behind
	out, err := exec.Command("git", "-C", projectRoot, "rev-list", "--count", "HEAD..origin/main").Output()
	if err != nil {
		return "", nil
	}

	behind, _ := strconv.Atoi(strings.TrimSpace(string(out)))
	if behind == 0 {
		return "", nil
	}

	// Attempt rebase
	rebaseCmd := exec.Command("git", "-C", projectRoot, "rebase", "origin/main", "--quiet")
	if err := rebaseCmd.Run(); err != nil {
		// Rebase failed — abort and block
		exec.Command("git", "-C", projectRoot, "rebase", "--abort").Run() //nolint:errcheck
		return "", fmt.Errorf("차단: origin/main rebase 중 충돌 발생. 수동으로 rebase 해결 후 다시 시도하세요.\n  git fetch origin main && git rebase origin/main")
	}

	return fmt.Sprintf("[enforce-rebase] origin/main 기준 rebase 완료 (%d개 커밋 반영)", behind), nil
}

func isGitPush(cmd string) bool {
	return strings.Contains(cmd, "git") && strings.Contains(cmd, "push")
}

func isGHPRCreate(cmd string) bool {
	return strings.Contains(cmd, "gh") && strings.Contains(cmd, "pr") && strings.Contains(cmd, "create")
}

func gitCurrentBranch(projectRoot string) (string, error) {
	out, err := exec.Command("git", "-C", projectRoot, "rev-parse", "--abbrev-ref", "HEAD").Output()
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(out)), nil
}
