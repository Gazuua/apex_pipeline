// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/cleanup"
	hookpkg "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
)

// runGitInRepo runs a git command in dir, failing the test on error.
func runGitInRepo(t *testing.T, dir string, args ...string) string {
	t.Helper()
	fullArgs := append([]string{"-C", dir}, args...)
	cmd := exec.Command("git", fullArgs...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v failed: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

// runGitInRepoNoFail runs a git command but does not fail on error — returns (output, err).
func runGitInRepoNoFail(dir string, args ...string) (string, error) {
	fullArgs := append([]string{"-C", dir}, args...)
	cmd := exec.Command("git", fullArgs...)
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

// TestEnforceRebase_AutoAndConflict verifies the rebase enforcement logic
// for both the "auto-rebase succeeds" path and the "conflict → abort" path.
//
// Scenario A — behind with clean rebase:
//  1. Create a temp repo with an initial commit on main.
//  2. Create a feature branch from main, add a commit.
//  3. Switch back to main, add a different file (no overlap → no conflict).
//  4. Switch to feature, confirm branch is behind origin/main simulation.
//  5. Call EnforceRebase("git push", repoDir).
//  6. Expect: message containing "rebase 완료", no error.
//
// Scenario B — conflicting changes → abort:
//  1. Same file modified on both main and feature at the same line.
//  2. Call EnforceRebase("git push", repoDir).
//  3. Expect: error containing "충돌".
func TestEnforceRebase_AutoAndConflict(t *testing.T) {
	env := testenv.New(t)
	repoDir := setupRebaseTestRepo(t, env)

	t.Run("auto_rebase_succeeds", func(t *testing.T) {
		// Create an isolated clone of the repo setup for this sub-test.
		cleanDir := setupRebaseScenario_Clean(t, env)

		msg, err := hookpkg.EnforceRebase("git push origin", cleanDir)
		if err != nil {
			t.Fatalf("EnforceRebase: expected no error on clean rebase, got: %v", err)
		}
		if msg == "" {
			t.Error("EnforceRebase: expected non-empty success message")
		}
		if !strings.Contains(msg, "rebase 완료") {
			t.Errorf("EnforceRebase: expected message to contain '재rebase 완료', got: %q", msg)
		}
	})

	t.Run("conflict_blocks", func(t *testing.T) {
		conflictDir := setupRebaseScenario_Conflict(t, env)

		_, err := hookpkg.EnforceRebase("git push origin", conflictDir)
		if err == nil {
			t.Fatal("EnforceRebase: expected error on conflicting rebase, got nil")
		}
		if !strings.Contains(err.Error(), "충돌") {
			t.Errorf("EnforceRebase: expected error to contain '충돌', got: %q", err.Error())
		}
	})

	t.Run("non_push_command_noop", func(t *testing.T) {
		// Commands that are not git push / gh pr create must be no-ops.
		msg, err := hookpkg.EnforceRebase("cat README.md", repoDir)
		if err != nil {
			t.Errorf("EnforceRebase on non-push cmd: unexpected error: %v", err)
		}
		if msg != "" {
			t.Errorf("EnforceRebase on non-push cmd: expected empty msg, got %q", msg)
		}
	})

	t.Run("main_branch_skipped", func(t *testing.T) {
		// On main/master branch itself, enforce-rebase is a no-op.
		// Create an isolated repo so we don't conflict with setupRebaseTestRepo's repo/ dir.
		mainRepoDir := filepath.Join(env.Dir, "main-skip-repo")
		if err := os.MkdirAll(mainRepoDir, 0o755); err != nil {
			t.Fatalf("mkdir main-skip-repo: %v", err)
		}
		runGitInRepo(t, mainRepoDir, "init", "-b", "main")
		runGitInRepo(t, mainRepoDir, "config", "user.email", "test@test.com")
		runGitInRepo(t, mainRepoDir, "config", "user.name", "Test")
		os.WriteFile(filepath.Join(mainRepoDir, "README.md"), []byte("# test\n"), 0o644)
		runGitInRepo(t, mainRepoDir, "add", ".")
		runGitInRepo(t, mainRepoDir, "commit", "-m", "initial")

		// HEAD is on main — enforce-rebase should be a no-op.
		msg, err := hookpkg.EnforceRebase("git push origin main", mainRepoDir)
		if err != nil {
			t.Errorf("EnforceRebase on main branch: unexpected error: %v", err)
		}
		if msg != "" {
			t.Errorf("EnforceRebase on main branch: expected empty msg, got %q", msg)
		}
	})
}

// setupRebaseTestRepo initialises a git repository with an initial commit on main.
// This is the shared base — individual sub-tests use their own copies.
func setupRebaseTestRepo(t *testing.T, env *testenv.TestEnv) string {
	t.Helper()
	return env.InitGitRepo(t)
}

// setupRebaseScenario_Clean creates a repo where the feature branch is behind
// origin/main by one commit with no overlapping file changes.
//
//   main:    A → B (new file: main_only.txt)
//   feature: A → C (new file: feature_only.txt)
//
// The test switches HEAD to feature so EnforceRebase sees "behind" and can rebase cleanly.
func setupRebaseScenario_Clean(t *testing.T, env *testenv.TestEnv) string {
	t.Helper()

	// Use a subdirectory to avoid collisions between sub-tests.
	repoDir := filepath.Join(env.Dir, "clean-rebase")
	if err := os.MkdirAll(repoDir, 0o755); err != nil {
		t.Fatalf("mkdir clean-rebase: %v", err)
	}

	runGitInRepo(t, repoDir, "init", "-b", "main")
	runGitInRepo(t, repoDir, "config", "user.email", "test@test.com")
	runGitInRepo(t, repoDir, "config", "user.name", "Test")

	// Commit A (base on main).
	os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("# base\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "initial")

	// Create feature branch from main.
	runGitInRepo(t, repoDir, "checkout", "-b", "feature/clean")
	os.WriteFile(filepath.Join(repoDir, "feature_only.txt"), []byte("feature\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "feature commit")

	// Switch back to main, add a non-overlapping commit.
	runGitInRepo(t, repoDir, "checkout", "main")
	os.WriteFile(filepath.Join(repoDir, "main_only.txt"), []byte("main\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "main extra commit")

	// Simulate "origin/main" by creating a local remote ref
	// EnforceRebase uses "origin/main" as target.
	// We create a bare clone to act as origin.
	bareDir := filepath.Join(env.Dir, "clean-bare")
	cmd := exec.Command("git", "clone", "--bare", repoDir, bareDir)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git clone --bare: %v\n%s", err, out)
	}

	// Add the bare clone as the "origin" remote of the feature branch repo.
	runGitInRepo(t, repoDir, "remote", "add", "origin", bareDir)
	runGitInRepo(t, repoDir, "fetch", "origin")

	// Switch to feature branch (this is where enforce-rebase will check).
	runGitInRepo(t, repoDir, "checkout", "feature/clean")

	// Confirm feature is behind origin/main.
	out, _ := runGitInRepoNoFail(repoDir, "rev-list", "--count", "HEAD..origin/main")
	behind := strings.TrimSpace(out)
	if behind == "0" || behind == "" {
		t.Skip("feature/clean is not behind origin/main — git config may differ; skipping")
	}

	return repoDir
}

// setupRebaseScenario_Conflict creates a repo where both main and feature
// modify the same file at the same line, causing a rebase conflict.
//
//   main:    A → B (file.txt line 1 = "main version")
//   feature: A → C (file.txt line 1 = "feature version")
func setupRebaseScenario_Conflict(t *testing.T, env *testenv.TestEnv) string {
	t.Helper()

	repoDir := filepath.Join(env.Dir, "conflict-rebase")
	if err := os.MkdirAll(repoDir, 0o755); err != nil {
		t.Fatalf("mkdir conflict-rebase: %v", err)
	}

	runGitInRepo(t, repoDir, "init", "-b", "main")
	runGitInRepo(t, repoDir, "config", "user.email", "test@test.com")
	runGitInRepo(t, repoDir, "config", "user.name", "Test")

	// Commit A: shared base file.
	os.WriteFile(filepath.Join(repoDir, "conflict.txt"), []byte("original line\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "initial")

	// Feature branch: modifies the same line.
	runGitInRepo(t, repoDir, "checkout", "-b", "feature/conflict")
	os.WriteFile(filepath.Join(repoDir, "conflict.txt"), []byte("feature version\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "feature changes conflict.txt")

	// Main: also modifies the same line (different content → guaranteed conflict).
	runGitInRepo(t, repoDir, "checkout", "main")
	os.WriteFile(filepath.Join(repoDir, "conflict.txt"), []byte("main version\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "main changes conflict.txt")

	// Set up a bare origin containing the latest main.
	bareDir := filepath.Join(env.Dir, "conflict-bare")
	cmd := exec.Command("git", "clone", "--bare", repoDir, bareDir)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git clone --bare: %v\n%s", err, out)
	}

	runGitInRepo(t, repoDir, "remote", "add", "origin", bareDir)
	runGitInRepo(t, repoDir, "fetch", "origin")

	// Switch to feature branch.
	runGitInRepo(t, repoDir, "checkout", "feature/conflict")

	// Verify feature is behind origin/main.
	out, _ := runGitInRepoNoFail(repoDir, "rev-list", "--count", "HEAD..origin/main")
	behind := strings.TrimSpace(out)
	if behind == "0" || behind == "" {
		t.Skip("feature/conflict is not behind origin/main; skipping conflict scenario")
	}

	return repoDir
}

// TestCleanup_MergedBranchDetection verifies that cleanup correctly identifies
// merged branches via ancestry check, and that Run in dry-run mode reports them.
//
//  1. Create a temp git repo with main + feature branch.
//  2. Commit unique changes on feature branch.
//  3. Merge feature into main (regular merge → ancestor check succeeds).
//  4. IsMergedToMain → true for the feature branch.
//  5. Create another unmerged branch → IsMergedToMain → false.
//  6. Run(repoDir, false) dry-run → merged branch appears in Local list.
func TestCleanup_MergedBranchDetection(t *testing.T) {
	env := testenv.New(t)

	repoDir := filepath.Join(env.Dir, "cleanup-test")
	if err := os.MkdirAll(repoDir, 0o755); err != nil {
		t.Fatalf("mkdir cleanup-test: %v", err)
	}

	runGitInRepo(t, repoDir, "init", "-b", "main")
	runGitInRepo(t, repoDir, "config", "user.email", "test@test.com")
	runGitInRepo(t, repoDir, "config", "user.name", "Test")
	runGitInRepo(t, repoDir, "config", "merge.ff", "false") // ensure real merge commits

	// Commit A on main.
	os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("# repo\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "initial")

	// Create and commit on feature/merged.
	runGitInRepo(t, repoDir, "checkout", "-b", "feature/merged")
	os.WriteFile(filepath.Join(repoDir, "merged.txt"), []byte("merged content\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "feature: add merged.txt")

	// Merge feature/merged into main.
	runGitInRepo(t, repoDir, "checkout", "main")
	runGitInRepo(t, repoDir, "merge", "feature/merged", "--no-edit")

	// Create an unmerged branch (NOT merged into main).
	runGitInRepo(t, repoDir, "checkout", "-b", "feature/unmerged")
	os.WriteFile(filepath.Join(repoDir, "unmerged.txt"), []byte("not yet merged\n"), 0o644)
	runGitInRepo(t, repoDir, "add", ".")
	runGitInRepo(t, repoDir, "commit", "-m", "feature: add unmerged.txt")

	// Switch back to main for the checks.
	runGitInRepo(t, repoDir, "checkout", "main")

	// Subtests.

	t.Run("IsMergedToMain_merged", func(t *testing.T) {
		if !cleanup.IsMergedToMain(repoDir, "feature/merged") {
			t.Error("IsMergedToMain: expected true for merged branch, got false")
		}
	})

	t.Run("IsMergedToMain_unmerged", func(t *testing.T) {
		if cleanup.IsMergedToMain(repoDir, "feature/unmerged") {
			t.Error("IsMergedToMain: expected false for unmerged branch, got true")
		}
	})

	t.Run("Run_dryrun_detects_merged", func(t *testing.T) {
		// Dry-run: Run will attempt `git fetch --prune origin` but there's no remote —
		// the function is tolerant of that (non-fatal). Phase 3 (remote) will be empty.
		// Phase 2 (local branches) should detect feature/merged.
		result, err := cleanup.Run(repoDir, false /* dry-run */)
		if err != nil {
			t.Fatalf("cleanup.Run dry-run: %v", err)
		}

		// Feature/merged should appear in Local list.
		foundMerged := false
		for _, action := range result.Local {
			if action.Branch == "feature/merged" {
				foundMerged = true
				if action.Done {
					t.Error("dry-run: expected Done=false (not executed), got true")
				}
			}
		}
		if !foundMerged {
			t.Errorf("cleanup.Run: expected feature/merged in Local list, got: %+v", result.Local)
		}

		// Feature/unmerged should appear in warnings (not in Local).
		for _, action := range result.Local {
			if action.Branch == "feature/unmerged" {
				t.Errorf("cleanup.Run: feature/unmerged unexpectedly in Local list (should be warning)")
			}
		}
	})

	t.Run("Run_execute_removes_merged", func(t *testing.T) {
		// Verify that executing cleanup actually removes the merged branch.
		result, err := cleanup.Run(repoDir, true /* execute */)
		if err != nil {
			t.Fatalf("cleanup.Run execute: %v", err)
		}

		// Feature/merged should be in Local with Done=true.
		foundMerged := false
		for _, action := range result.Local {
			if action.Branch == "feature/merged" {
				foundMerged = true
				if !action.Done {
					t.Error("execute: expected Done=true, got false")
				}
			}
		}
		if !foundMerged {
			t.Errorf("cleanup.Run execute: feature/merged not in Local list: %+v", result.Local)
		}

		// Verify the branch is actually gone.
		_, err = runGitInRepoNoFail(repoDir, "rev-parse", "--verify", "feature/merged")
		if err == nil {
			t.Error("feature/merged should have been deleted but still exists in git")
		}
	})
}
