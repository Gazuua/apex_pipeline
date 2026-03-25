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

func TestValidateBuild_AllowApexAgentBuildBat(t *testing.T) {
	cmds := []string{
		"bash apex_tools/apex-agent/build.bat",
		"bash ./apex_tools/apex-agent/build.bat",
		"apex_tools/apex-agent/build.bat test",
		"bash /d/.workspace/apex_pipeline/apex_tools/apex-agent/build.bat",
		`cmd.exe //c "D:\.workspace\apex_pipeline\apex_tools\apex-agent\build.bat"`,
		`cmd.exe //c "C:\Users\JHG\project\apex_tools\apex-agent\build.bat"`,
	}
	for _, cmd := range cmds {
		if err := ValidateBuild(cmd); err != nil {
			t.Errorf("apex-agent build.bat %q should be allowed, got: %v", cmd, err)
		}
	}
	// 루트 build.bat은 여전히 차단
	if err := ValidateBuild("build.bat debug"); err == nil {
		t.Error("root build.bat should still be blocked")
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

// ── Git branch creation blocking ──

func TestValidateBuild_BlocksGitBranchCreation(t *testing.T) {
	blocked := []string{
		"git checkout -b feature/foo",
		"git checkout -B feature/foo",
		"git switch -c feature/foo",
		"git switch --create feature/foo",
		"git branch feature/foo",
		"git worktree add -b feature/foo ../worktree",
	}
	for _, cmd := range blocked {
		if err := ValidateBuild(cmd); err == nil {
			t.Errorf("expected block for %q", cmd)
		}
	}
}

func TestValidateBuild_AllowsGitBranchQueries(t *testing.T) {
	allowed := []string{
		"git branch",
		"git branch -a",
		"git branch -v",
		"git branch --list",
		"git branch -D feature/foo",
		"git branch -d feature/foo",
		"git checkout main",
		"git checkout feature/existing",
		"git status",
		"git push origin --delete feature/foo",
	}
	for _, cmd := range allowed {
		if err := ValidateBuild(cmd); err != nil {
			t.Errorf("unexpected block for %q: %v", cmd, err)
		}
	}
}

// ── ValidateBacklog ──

func TestValidateBacklog_BlocksBacklogMD(t *testing.T) {
	err := ValidateBacklog("/project/docs/BACKLOG.md")
	if err == nil {
		t.Fatal("expected block for BACKLOG.md")
	}
}

func TestValidateBacklog_BlocksHistoryMD(t *testing.T) {
	err := ValidateBacklog("/project/docs/BACKLOG_HISTORY.md")
	if err == nil {
		t.Fatal("expected block for BACKLOG_HISTORY.md")
	}
}

func TestValidateBacklog_AllowsOtherFiles(t *testing.T) {
	err := ValidateBacklog("/project/docs/CLAUDE.md")
	if err != nil {
		t.Errorf("should allow other files, got: %v", err)
	}
}

func TestValidateBacklog_AllowsSubdirBacklog(t *testing.T) {
	err := ValidateBacklog("/project/docs/apex_tools/BACKLOG.md")
	if err != nil {
		t.Errorf("should allow subdirectory backlog, got: %v", err)
	}
}

func TestValidateBacklog_CaseInsensitiveBlocks(t *testing.T) {
	// Case-insensitive matching to prevent Windows bypass (NTFS is case-insensitive).
	err := ValidateBacklog("/project/docs/backlog.md")
	if err == nil {
		t.Fatal("expected block for lowercase backlog.md")
	}
	err = ValidateBacklog("/project/docs/Backlog.Md")
	if err == nil {
		t.Fatal("expected block for mixed-case Backlog.Md")
	}
}

func TestValidateBacklog_WindowsPath(t *testing.T) {
	err := ValidateBacklog(`D:\project\docs\BACKLOG.md`)
	if err == nil {
		t.Fatal("expected block for Windows path BACKLOG.md")
	}
}
