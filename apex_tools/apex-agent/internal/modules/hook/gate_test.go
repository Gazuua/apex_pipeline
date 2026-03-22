// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
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

func TestValidateBuild_AllowApexAgent(t *testing.T) {
	cmds := []string{
		"apex-agent queue build debug",
		"apex-agent queue benchmark bench_mpsc_queue.exe",
		`/d/.workspace/apex_tools/apex-agent/run-hook queue build debug`,
		`bash ./apex_tools/apex-agent/run-hook queue benchmark bench_mpsc_queue.exe --benchmark_format=json`,
	}
	for _, cmd := range cmds {
		if err := ValidateBuild(cmd); err != nil {
			t.Errorf("apex-agent/run-hook command %q should be allowed, got: %v", cmd, err)
		}
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

func TestValidateBuild_AllowGoToolchain(t *testing.T) {
	cmds := []string{
		"go build ./...",
		"go test ./internal/modules/hook/... -v",
		"go run ./cmd/apex-agent version",
		"go install ./cmd/apex-agent",
		`export PATH="/c/Program Files/Go/bin:$PATH" && go build -o apex-agent.exe ./cmd/apex-agent`,
	}
	for _, cmd := range cmds {
		if err := ValidateBuild(cmd); err != nil {
			t.Errorf("go command %q should be allowed, got: %v", cmd, err)
		}
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

// ValidateMerge tests removed — file-based lock replaced by daemon IPC (hook_cmd.go).
// Merge lock validation is now tested via E2E (TestHook_ValidateMergeRequiresLock).
