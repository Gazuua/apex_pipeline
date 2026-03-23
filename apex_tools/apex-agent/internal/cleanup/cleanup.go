// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cleanup

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("cleanup")

// Result holds the outcome of a cleanup run.
type Result struct {
	Worktrees []Action
	Local     []Action
	EmptyDirs []Action
	Remote    []Action
	Warnings  []string
}

// Action describes a single cleanup target and whether it was executed.
type Action struct {
	Target string // filesystem path or branch name
	Branch string // associated branch (may be empty for empty-dir actions)
	Done   bool   // true when execute=true and deletion succeeded
}

var protectedBranches = map[string]bool{
	"main":     true,
	"master":   true,
	"gh-pages": true,
}

// Run performs the 3-phase branch cleanup.
// activeBranches contains git branch names that have active handoff records — these are skipped.
// If execute is false the run is a dry-run: all targets are collected and
// returned but nothing is deleted.
func Run(repoRoot string, execute bool, activeBranches map[string]bool) (*Result, error) {
	// Resolve repo root from git if not provided.
	if repoRoot == "" {
		out, err := runGit(repoRoot, "rev-parse", "--show-toplevel")
		if err != nil {
			return nil, fmt.Errorf("git rev-parse --show-toplevel: %w", err)
		}
		repoRoot = strings.TrimSpace(out)
	}
	if activeBranches == nil {
		activeBranches = map[string]bool{}
	}

	result := &Result{}
	ml.Info("cleanup started", "repo", repoRoot, "execute", execute, "active_branches", len(activeBranches))

	// Refresh remote refs.
	_, _ = runGit(repoRoot, "fetch", "--prune", "origin")

	// Phase 1: worktrees.
	dirtyBranches, err := processWorktrees(repoRoot, execute, activeBranches, result)
	if err != nil {
		return nil, fmt.Errorf("worktree phase: %w", err)
	}

	// Phase 2: local branches.
	if err := processLocalBranches(repoRoot, execute, dirtyBranches, activeBranches, result); err != nil {
		return nil, fmt.Errorf("local branch phase: %w", err)
	}

	// Phase 2.5: empty worktree directories.
	processEmptyWorktreeDirs(repoRoot, execute, result)

	// Phase 3: remote branches.
	if err := processRemoteBranches(repoRoot, execute, activeBranches, result); err != nil {
		return nil, fmt.Errorf("remote branch phase: %w", err)
	}

	return result, nil
}

// ── Phase 1: Worktrees ────────────────────────────────────────────────────────

// processWorktrees lists git worktrees and removes merged, clean ones.
// Returns the set of branches that have dirty worktrees (for Phase 2 skip).
func processWorktrees(repoRoot string, execute bool, activeBranches map[string]bool, result *Result) (map[string]bool, error) {
	dirtyBranches := map[string]bool{}
	cwd, _ := os.Getwd()

	out, err := runGit(repoRoot, "worktree", "list")
	if err != nil {
		return dirtyBranches, nil // non-fatal; repo may have no extra worktrees
	}

	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		wtPath, wtBranch := parseWorktreeLine(line)
		if wtBranch == "" || protectedBranches[wtBranch] {
			continue
		}

		// Defense-in-depth: CWD protection.
		if isSubPath(cwd, wtPath) {
			result.Warnings = append(result.Warnings,
				fmt.Sprintf("현재 작업 디렉토리: %s [%s] — 스킵", wtPath, wtBranch))
			continue
		}

		// Active handoff protection.
		if activeBranches[wtBranch] {
			result.Warnings = append(result.Warnings,
				fmt.Sprintf("활성 핸드오프: %s [%s] — 스킵", wtPath, wtBranch))
			continue
		}

		if !IsMergedToMain(repoRoot, wtBranch) {
			result.Warnings = append(result.Warnings,
				fmt.Sprintf("미머지 워크트리: %s [%s]", wtPath, wtBranch))
			continue
		}

		// Skip dirty worktrees.
		if isDirtyWorktree(wtPath) {
			result.Warnings = append(result.Warnings,
				fmt.Sprintf("변경사항 있음 (워크트리): %s [%s]", wtPath, wtBranch))
			dirtyBranches[wtBranch] = true
			continue
		}

		action := Action{Target: wtPath, Branch: wtBranch}
		if execute {
			if err := removeWorktree(repoRoot, wtPath); err != nil {
				ml.Warn("worktree removal failed", "path", wtPath, "branch", wtBranch, "err", err)
				result.Warnings = append(result.Warnings,
					fmt.Sprintf("워크트리 삭제 실패: %s — %v", wtPath, err))
			} else {
				ml.Info("worktree removed", "path", wtPath, "branch", wtBranch)
				action.Done = true
			}
		}
		result.Worktrees = append(result.Worktrees, action)
	}

	return dirtyBranches, nil
}

// isSubPath returns true if child is inside or equal to parent.
// Handles Windows case-insensitivity and drive letter normalization.
func isSubPath(child, parent string) bool {
	absChild, err1 := filepath.Abs(child)
	absParent, err2 := filepath.Abs(parent)
	if err1 != nil || err2 != nil {
		return false
	}
	if runtime.GOOS == "windows" {
		absChild = strings.ToLower(absChild)
		absParent = strings.ToLower(absParent)
	}
	rel, err := filepath.Rel(absParent, absChild)
	if err != nil {
		return false
	}
	return !strings.HasPrefix(rel, "..")
}

// parseWorktreeLine extracts the path and branch from a `git worktree list` output line.
// Format: /path/to/worktree  <hash>  [branch]
func parseWorktreeLine(line string) (path, branch string) {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return "", ""
	}
	path = fields[0]
	// Find bracketed branch name.
	start := strings.Index(line, "[")
	end := strings.Index(line, "]")
	if start >= 0 && end > start {
		branch = line[start+1 : end]
	}
	return path, branch
}

// isDirtyWorktree returns true when the worktree at path has uncommitted changes or untracked files.
func isDirtyWorktree(wtPath string) bool {
	if _, err := os.Stat(wtPath); err != nil {
		return false
	}
	out, err := runGit(wtPath, "status", "--porcelain")
	if err != nil {
		return false
	}
	return strings.TrimSpace(out) != ""
}

// removeWorktree forcefully removes a worktree and any leftover directory.
func removeWorktree(repoRoot, wtPath string) error {
	_, gitErr := runGit(repoRoot, "worktree", "remove", "--force", wtPath)
	if gitErr != nil {
		// Best-effort fallback: manual removal.
		_ = os.RemoveAll(wtPath)
	}
	// Remove any residual directory.
	if info, statErr := os.Stat(wtPath); statErr == nil && info.IsDir() {
		rmErr := os.RemoveAll(wtPath)
		// If directory still exists after both attempts, return error.
		if rmErr != nil {
			if _, stillExists := os.Stat(wtPath); stillExists == nil {
				return fmt.Errorf("removeWorktree: directory still exists after removal attempts: %s (git: %v, rm: %w)", wtPath, gitErr, rmErr)
			}
		}
	}
	return nil
}

// ── Phase 2: Local Branches ───────────────────────────────────────────────────

func processLocalBranches(repoRoot string, execute bool, dirtyBranches, activeBranches map[string]bool, result *Result) error {
	out, err := runGit(repoRoot, "branch")
	if err != nil {
		return nil // non-fatal
	}

	for _, line := range strings.Split(out, "\n") {
		// Strip leading *, +, and spaces.
		branch := strings.TrimLeft(line, "*+ ")
		branch = strings.TrimSpace(branch)
		if branch == "" || protectedBranches[branch] {
			continue
		}

		// Active handoff protection.
		if activeBranches[branch] {
			result.Warnings = append(result.Warnings, fmt.Sprintf("활성 핸드오프: %s — 스킵", branch))
			continue
		}

		if !IsMergedToMain(repoRoot, branch) {
			result.Warnings = append(result.Warnings, fmt.Sprintf("미머지 로컬: %s", branch))
			continue
		}

		if dirtyBranches[branch] {
			result.Warnings = append(result.Warnings, fmt.Sprintf("변경사항 있음 (워크트리): %s", branch))
			continue
		}

		action := Action{Target: branch, Branch: branch}
		if execute {
			if _, delErr := runGit(repoRoot, "branch", "-D", branch); delErr != nil {
				result.Warnings = append(result.Warnings,
					fmt.Sprintf("로컬 브랜치 삭제 실패: %s — %v", branch, delErr))
			} else {
				action.Done = true
				// Also clean up residual worktree directories.
				cleanResidualWorktreeDirs(repoRoot, branch)
			}
		}
		result.Local = append(result.Local, action)
	}
	return nil
}

// cleanResidualWorktreeDirs removes leftover .worktrees/ subdirectories for a branch.
func cleanResidualWorktreeDirs(repoRoot, branch string) {
	wtDirFull := filepath.Join(repoRoot, ".worktrees", branch)
	// Short form: strip prefix up to the last slash component.
	shortName := branch
	if idx := strings.LastIndex(branch, "/"); idx >= 0 {
		shortName = branch[idx+1:]
	}
	wtDirShort := filepath.Join(repoRoot, ".worktrees", shortName)

	seen := map[string]bool{}
	for _, dir := range []string{wtDirFull, wtDirShort} {
		if seen[dir] {
			continue
		}
		seen[dir] = true
		if info, err := os.Stat(dir); err == nil && info.IsDir() {
			_ = os.RemoveAll(dir)
		}
	}
}

// ── Phase 2.5: Empty worktree directories ────────────────────────────────────

func processEmptyWorktreeDirs(repoRoot string, execute bool, result *Result) {
	wtRoot := filepath.Join(repoRoot, ".worktrees")
	entries, err := os.ReadDir(wtRoot)
	if err != nil {
		return // .worktrees doesn't exist — nothing to do
	}

	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		dir := filepath.Join(wtRoot, entry.Name())
		children, err := os.ReadDir(dir)
		if err != nil || len(children) > 0 {
			continue
		}
		// Empty directory.
		action := Action{Target: dir}
		if execute {
			if removeErr := os.RemoveAll(dir); removeErr == nil {
				action.Done = true
			}
		}
		result.EmptyDirs = append(result.EmptyDirs, action)
	}
}

// ── Phase 3: Remote Branches ─────────────────────────────────────────────────

func processRemoteBranches(repoRoot string, execute bool, activeBranches map[string]bool, result *Result) error {
	if execute {
		// Prune stale remote refs first.
		_, _ = runGit(repoRoot, "remote", "prune", "origin")
	}

	out, err := runGit(repoRoot, "branch", "-r")
	if err != nil {
		return nil // non-fatal
	}

	for _, line := range strings.Split(out, "\n") {
		ref := strings.TrimSpace(line)
		if ref == "" || strings.Contains(ref, "->") {
			continue
		}

		// Strip "origin/" prefix.
		if !strings.HasPrefix(ref, "origin/") {
			continue
		}
		branch := strings.TrimPrefix(ref, "origin/")

		if protectedBranches[branch] {
			continue
		}

		// Active handoff protection.
		if activeBranches[branch] {
			result.Warnings = append(result.Warnings, fmt.Sprintf("활성 핸드오프: origin/%s — 스킵", branch))
			continue
		}

		if IsMergedToMain(repoRoot, "origin/"+branch) {
			action := Action{Target: "origin/" + branch, Branch: branch}
			if execute {
				if _, delErr := runGit(repoRoot, "push", "origin", "--delete", branch); delErr != nil {
					result.Warnings = append(result.Warnings,
						fmt.Sprintf("리모트 브랜치 삭제 실패: origin/%s — %v", branch, delErr))
				} else {
					action.Done = true
				}
			}
			result.Remote = append(result.Remote, action)
		} else {
			result.Warnings = append(result.Warnings, fmt.Sprintf("미머지 리모트: origin/%s", branch))
		}
	}
	return nil
}

// ── Merge Detection ───────────────────────────────────────────────────────────

// IsMergedToMain checks if branch has been merged to main using 3-layer detection:
//  1. git merge-base --is-ancestor (regular merge)
//  2. gh pr list --state merged (squash merge via GitHub PR)
//  3. Blob hash comparison (squash merge without a PR)
func IsMergedToMain(repoRoot, branch string) bool {
	// Layer 1: ancestor check.
	cmd := exec.Command("git", "-C", repoRoot, "merge-base", "--is-ancestor", branch, "main")
	if cmd.Run() == nil {
		return true
	}

	// Layer 2: GitHub PR check (squash merges).
	ghBranch := strings.TrimPrefix(branch, "origin/")
	if ghAvailable() {
		out, err := exec.Command("gh", "pr", "list",
			"--head", ghBranch,
			"--state", "merged",
			"--json", "number",
			"--jq", "length").Output()
		if err == nil {
			count := strings.TrimSpace(string(out))
			if count != "" && count != "0" {
				return true
			}
		}
	}

	// Layer 3: blob hash comparison.
	return blobHashMatch(repoRoot, branch)
}

// ghAvailable returns true when the gh CLI binary is on PATH.
func ghAvailable() bool {
	_, err := exec.LookPath("gh")
	return err == nil
}

// blobHashMatch returns true when every file changed between branch and its
// merge-base with origin/main has the same blob hash on origin/main.
// This catches squash merges that were not recorded as GitHub PRs.
func blobHashMatch(repoRoot, branch string) bool {
	mergeBaseOut, err := exec.Command("git", "-C", repoRoot,
		"merge-base", branch, "origin/main").Output()
	if err != nil {
		return false
	}
	mergeBase := strings.TrimSpace(string(mergeBaseOut))
	if mergeBase == "" {
		return false
	}

	changedOut, err := exec.Command("git", "-C", repoRoot,
		"diff", "--name-only", mergeBase, branch).Output()
	if err != nil {
		return false
	}
	changedFiles := strings.Split(strings.TrimSpace(string(changedOut)), "\n")
	if len(changedFiles) == 0 || (len(changedFiles) == 1 && changedFiles[0] == "") {
		return false
	}

	for _, file := range changedFiles {
		if file == "" {
			continue
		}
		branchBlob, err1 := exec.Command("git", "-C", repoRoot,
			"rev-parse", branch+":"+file).Output()
		mainBlob, err2 := exec.Command("git", "-C", repoRoot,
			"rev-parse", "origin/main:"+file).Output()
		if err1 != nil || err2 != nil {
			return false
		}
		if strings.TrimSpace(string(branchBlob)) != strings.TrimSpace(string(mainBlob)) {
			return false
		}
	}
	return true
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// runGit runs a git command in dir and returns stdout.
func runGit(dir string, args ...string) (string, error) {
	// If the first arg is "-C" we're already passing a working directory override,
	// so don't prepend it again.
	var cmd *exec.Cmd
	if len(args) > 0 && args[0] == "-C" {
		cmd = exec.Command("git", args...)
	} else {
		fullArgs := append([]string{"-C", dir}, args...)
		cmd = exec.Command("git", fullArgs...)
	}
	out, err := cmd.Output()
	return string(out), err
}
