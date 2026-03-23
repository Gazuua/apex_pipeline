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

// ── ValidateNewBranch ──

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
	if !strings.Contains(err.Error(), "로컬") {
		t.Errorf("expected '로컬' in error, got: %q", err.Error())
	}
}

// ── CreateAndPushBranch ──

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

// ── RebaseOnMain ──

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
	runGit(t, repoDir, "checkout", "-b", "feature/clean")
	os.WriteFile(filepath.Join(repoDir, "feature.txt"), []byte("feature\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "feature commit")
	// main에 별도 파일 추가 (충돌 없음)
	runGit(t, repoDir, "checkout", "main")
	os.WriteFile(filepath.Join(repoDir, "main.txt"), []byte("main\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "main commit")
	runGit(t, repoDir, "push", bareDir, "main")
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

// ── CheckoutMain ──

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
