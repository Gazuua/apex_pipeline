// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"os"
	"path/filepath"
	"testing"
)

// ── ValidateBuild ──

func TestValidateBuild_EmptyCommand(t *testing.T) {
	if err := ValidateBuild(""); err != nil {
		t.Errorf("empty command should be allowed, got: %v", err)
	}
}

func TestValidateBuild_AllowQueueLock(t *testing.T) {
	if err := ValidateBuild(`/d/.workspace/apex_tools/queue-lock.sh build debug`); err != nil {
		t.Errorf("queue-lock.sh should be allowed, got: %v", err)
	}
}

func TestValidateBuild_AllowReadOnly(t *testing.T) {
	cmds := []string{"cat build.bat", "grep pattern file", "head -10 CMakeLists.txt", "echo hello"}
	for _, cmd := range cmds {
		if err := ValidateBuild(cmd); err != nil {
			t.Errorf("read-only %q should be allowed, got: %v", cmd, err)
		}
	}
}

func TestValidateBuild_BlockCmake(t *testing.T) {
	if err := ValidateBuild("cmake --build out/build"); err == nil {
		t.Error("cmake --build should be blocked")
	}
}

func TestValidateBuild_BlockNinja(t *testing.T) {
	if err := ValidateBuild("ninja -C out"); err == nil {
		t.Error("ninja should be blocked")
	}
}

func TestValidateBuild_BlockBuildBat(t *testing.T) {
	if err := ValidateBuild("build.bat"); err == nil {
		t.Error("build.bat should be blocked")
	}
}

func TestValidateBuild_BlockBenchmark(t *testing.T) {
	if err := ValidateBuild("./bench_dispatch"); err == nil {
		t.Error("bench_ prefixed binary should be blocked")
	}
}

func TestValidateBuild_AllowNormalCommand(t *testing.T) {
	cmds := []string{"git status", "go test ./...", "ls -la", "cd /tmp"}
	for _, cmd := range cmds {
		if err := ValidateBuild(cmd); err != nil {
			t.Errorf("normal command %q should be allowed, got: %v", cmd, err)
		}
	}
}

// ── ValidateMerge ──

func TestValidateMerge_EmptyCommand(t *testing.T) {
	if err := ValidateMerge("", ""); err != nil {
		t.Errorf("empty command should be allowed, got: %v", err)
	}
}

func TestValidateMerge_NonMergeCommand(t *testing.T) {
	if err := ValidateMerge("gh pr create", "/workspace"); err != nil {
		t.Errorf("non-merge command should be allowed, got: %v", err)
	}
}

func TestValidateMerge_BlockWithoutLock(t *testing.T) {
	// Use a temp directory that definitely doesn't have merge.lock.
	orig := os.Getenv("APEX_BUILD_QUEUE_DIR")
	tmpDir := t.TempDir()
	os.Setenv("APEX_BUILD_QUEUE_DIR", tmpDir)
	defer os.Setenv("APEX_BUILD_QUEUE_DIR", orig)

	err := ValidateMerge("gh pr merge --squash", "/workspace")
	if err == nil {
		t.Error("gh pr merge without lock should be blocked")
	}
}

func TestValidateMerge_AllowWithLock(t *testing.T) {
	tmpDir := t.TempDir()
	os.MkdirAll(filepath.Join(tmpDir, "merge.lock"), 0o755)

	orig := os.Getenv("APEX_BUILD_QUEUE_DIR")
	os.Setenv("APEX_BUILD_QUEUE_DIR", tmpDir)
	defer os.Setenv("APEX_BUILD_QUEUE_DIR", orig)

	err := ValidateMerge("gh pr merge --squash", "/workspace")
	if err != nil {
		t.Errorf("gh pr merge with lock should be allowed, got: %v", err)
	}
}

func TestValidateMerge_BlockWrongOwner(t *testing.T) {
	tmpDir := t.TempDir()
	os.MkdirAll(filepath.Join(tmpDir, "merge.lock"), 0o755)
	os.WriteFile(filepath.Join(tmpDir, "merge.owner"), []byte("BRANCH=branch_01\n"), 0o644)

	orig := os.Getenv("APEX_BUILD_QUEUE_DIR")
	os.Setenv("APEX_BUILD_QUEUE_DIR", tmpDir)
	defer os.Setenv("APEX_BUILD_QUEUE_DIR", orig)

	err := ValidateMerge("gh pr merge --squash", "/workspace/branch_02")
	if err == nil {
		t.Error("merge by non-owner should be blocked")
	}
}

func TestValidateMerge_AllowCorrectOwner(t *testing.T) {
	tmpDir := t.TempDir()
	os.MkdirAll(filepath.Join(tmpDir, "merge.lock"), 0o755)
	os.WriteFile(filepath.Join(tmpDir, "merge.owner"), []byte("BRANCH=branch_02\n"), 0o644)

	orig := os.Getenv("APEX_BUILD_QUEUE_DIR")
	os.Setenv("APEX_BUILD_QUEUE_DIR", tmpDir)
	defer os.Setenv("APEX_BUILD_QUEUE_DIR", orig)

	err := ValidateMerge("gh pr merge --squash", "/workspace/branch_02")
	if err != nil {
		t.Errorf("merge by correct owner should be allowed, got: %v", err)
	}
}
