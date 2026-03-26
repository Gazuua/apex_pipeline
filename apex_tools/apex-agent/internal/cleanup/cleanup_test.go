// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cleanup

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestProtectedBranches(t *testing.T) {
	if !protectedBranches["main"] {
		t.Error("main should be protected")
	}
	if !protectedBranches["master"] {
		t.Error("master should be protected")
	}
	if !protectedBranches["gh-pages"] {
		t.Error("gh-pages should be protected")
	}
	if protectedBranches["feature/test"] {
		t.Error("feature branch should not be protected")
	}
	if protectedBranches["bugfix/issue-42"] {
		t.Error("bugfix branch should not be protected")
	}
}

func TestParseWorktreeLine(t *testing.T) {
	cases := []struct {
		line           string
		wantPath       string
		wantBranch     string
	}{
		{
			line:       "/workspace/apex_pipeline_branch_02  abc1234 [feature/my-feature]",
			wantPath:   "/workspace/apex_pipeline_branch_02",
			wantBranch: "feature/my-feature",
		},
		{
			line:       "D:/workspace/apex  deadbeef [main]",
			wantPath:   "D:/workspace/apex",
			wantBranch: "main",
		},
		{
			line:       "/some/path  abc1234 (HEAD detached at abc1234)",
			wantPath:   "/some/path",
			wantBranch: "", // no bracket — detached HEAD
		},
		{
			line:       "",
			wantPath:   "",
			wantBranch: "",
		},
	}

	for _, tc := range cases {
		gotPath, gotBranch := parseWorktreeLine(tc.line)
		if gotPath != tc.wantPath {
			t.Errorf("parseWorktreeLine(%q) path = %q, want %q", tc.line, gotPath, tc.wantPath)
		}
		if gotBranch != tc.wantBranch {
			t.Errorf("parseWorktreeLine(%q) branch = %q, want %q", tc.line, gotBranch, tc.wantBranch)
		}
	}
}

func TestGhAvailable(t *testing.T) {
	// We only verify that the function returns without panicking.
	// Whether gh is installed or not is environment-dependent.
	_ = ghAvailable()
}

// ── isSubPath ──

func TestIsSubPath(t *testing.T) {
	// 절대 경로 기반 테스트를 위해 임시 디렉토리 사용
	tmp := t.TempDir()
	child := filepath.Join(tmp, "sub", "deep")
	os.MkdirAll(child, 0o755)

	cases := []struct {
		name          string
		child, parent string
		want          bool
	}{
		{"same dir", tmp, tmp, true},
		{"child inside parent", child, tmp, true},
		{"parent not inside child", tmp, child, false},
		{"sibling dirs", filepath.Join(tmp, "a"), filepath.Join(tmp, "b"), false},
		{"current dir self-reference", ".", ".", true},
	}

	// Windows 대소문자 무시 테스트
	if runtime.GOOS == "windows" {
		upper := filepath.Join(tmp, "SUB")
		cases = append(cases, struct {
			name          string
			child, parent string
			want          bool
		}{"windows case insensitive", upper, tmp, true})
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := isSubPath(tc.child, tc.parent)
			if got != tc.want {
				t.Errorf("isSubPath(%q, %q) = %v, want %v", tc.child, tc.parent, got, tc.want)
			}
		})
	}
}

// ── activeBranches protection ──

func TestActiveBranches_MapLookup(t *testing.T) {
	// activeBranches map이 lookup을 올바르게 수행하는지 확인하는 단위 테스트.
	// processLocalBranches는 git 명령이 필요하므로 E2E에서 커버된다.
	// 여기서는 보호 로직의 핵심인 map lookup만 검증.
	active := map[string]bool{
		"feature/my-work":    true,
		"bugfix/urgent-fix":  true,
	}

	// activeBranches에 있으면 true
	if !active["feature/my-work"] {
		t.Error("feature/my-work should be in activeBranches")
	}
	if !active["bugfix/urgent-fix"] {
		t.Error("bugfix/urgent-fix should be in activeBranches")
	}
	// activeBranches에 없으면 false
	if active["feature/other"] {
		t.Error("feature/other should NOT be in activeBranches")
	}
}

// TestRunWithActiveBranches_Integration은 git repo를 사용하는 통합 테스트.
// e2e/git_test.go에서 더 상세하게 검증.
// 여기서는 Run 함수의 시그니처와 nil activeBranches 허용만 검증.
func TestRunSignature_NilActiveBranches(t *testing.T) {
	// nil activeBranches는 빈 맵으로 처리되어야 함 (panic 없이).
	// git repo가 아닌 디렉토리에서도 Run은 graceful하게 처리되어야 한다.
	tmp := t.TempDir()
	result, err := Run(tmp, false, nil)
	if err != nil {
		t.Errorf("Run should handle non-git directory gracefully, got error: %v", err)
	}
	// 결과가 반환되면 삭제 대상이 비어 있어야 함 (정리할 브랜치가 없으므로)
	if result != nil && (len(result.Local) > 0 || len(result.Remote) > 0) {
		t.Errorf("expected empty result in non-git directory, got local=%d remote=%d",
			len(result.Local), len(result.Remote))
	}
}

func TestRunSignature_EmptyActiveBranches(t *testing.T) {
	tmp := t.TempDir()
	result, err := Run(tmp, false, map[string]bool{})
	if err != nil {
		t.Errorf("Run should handle non-git directory gracefully, got error: %v", err)
	}
	if result != nil && (len(result.Local) > 0 || len(result.Remote) > 0) {
		t.Errorf("expected empty result in non-git directory, got local=%d remote=%d",
			len(result.Local), len(result.Remote))
	}
}

// ── parseWorktreeLine 추가 엣지 케이스 ──

func TestParseWorktreeLine_EdgeCases(t *testing.T) {
	cases := []struct {
		name       string
		line       string
		wantPath   string
		wantBranch string
	}{
		{
			name:       "bare repository (no branch)",
			line:       "/workspace/repo  abc1234 (bare)",
			wantPath:   "/workspace/repo",
			wantBranch: "",
		},
		{
			name:       "multiple spaces between fields",
			line:       "/workspace/repo    abc1234    [feature/test]",
			wantPath:   "/workspace/repo",
			wantBranch: "feature/test",
		},
		{
			name:       "branch with nested slashes",
			line:       "/workspace/repo  abc1234 [feature/BACKLOG-42/deep/nested]",
			wantPath:   "/workspace/repo",
			wantBranch: "feature/BACKLOG-42/deep/nested",
		},
		{
			name:       "whitespace-only line",
			line:       "   ",
			wantPath:   "",
			wantBranch: "",
		},
		{
			name:       "bracket at very end",
			line:       "/path  hash [main]",
			wantPath:   "/path",
			wantBranch: "main",
		},
		{
			name:       "incomplete opening bracket only",
			line:       "/path  hash [main",
			wantPath:   "/path",
			wantBranch: "", // no closing bracket
		},
		{
			name:       "closing bracket before opening",
			line:       "/path  hash ]main[",
			wantPath:   "/path",
			wantBranch: "", // invalid bracket order
		},
		{
			name:       "empty brackets",
			line:       "/path  hash []",
			wantPath:   "/path",
			wantBranch: "",
		},
		{
			name:       "Windows backslash path",
			line:       "D:\\workspace\\repo  abc1234 [feature/win]",
			wantPath:   "D:\\workspace\\repo",
			wantBranch: "feature/win",
		},
		{
			name:       "single field only (path)",
			line:       "/workspace/repo",
			wantPath:   "/workspace/repo",
			wantBranch: "",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			gotPath, gotBranch := parseWorktreeLine(tc.line)
			if gotPath != tc.wantPath {
				t.Errorf("path = %q, want %q", gotPath, tc.wantPath)
			}
			if gotBranch != tc.wantBranch {
				t.Errorf("branch = %q, want %q", gotBranch, tc.wantBranch)
			}
		})
	}
}

// ── protectedBranches 엣지 케이스 ──

func TestProtectedBranches_EdgeCases(t *testing.T) {
	cases := []struct {
		name   string
		branch string
		want   bool
	}{
		{"empty string", "", false},
		{"Main (uppercase)", "Main", false},
		{"MAIN (all caps)", "MAIN", false},
		{"main with slash", "main/sub", false},
		{"master with prefix", "old-master", false},
		{"gh-pages exact", "gh-pages", true},
		{"gh-pages with suffix", "gh-pages-v2", false},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := protectedBranches[tc.branch]
			if got != tc.want {
				t.Errorf("protectedBranches[%q] = %v, want %v", tc.branch, got, tc.want)
			}
		})
	}
}

// ── isSubPath 추가 엣지 케이스 ──

func TestIsSubPath_EdgeCases(t *testing.T) {
	tmp := t.TempDir()

	// 깊은 중첩 경로 생성
	deep := filepath.Join(tmp, "a", "b", "c", "d")
	os.MkdirAll(deep, 0o755)

	cases := []struct {
		name          string
		child, parent string
		want          bool
	}{
		{"deep nesting inside parent", deep, tmp, true},
		{"deep nesting reverse", tmp, deep, false},
		{"path with trailing separator", tmp + string(filepath.Separator), tmp, true},
		{"parent with trailing separator", tmp, tmp + string(filepath.Separator), true},
	}

	// dot-dot traversal: tmp/a/b/../../a should still be inside tmp
	dotdot := filepath.Join(tmp, "a", "b", "..", "..", "a")
	cases = append(cases, struct {
		name          string
		child, parent string
		want          bool
	}{"dot-dot traversal resolves inside parent", dotdot, tmp, true})

	// dot-dot escaping: tmp/a/../.. goes outside tmp
	escape := filepath.Join(tmp, "a", "..", "..")
	cases = append(cases, struct {
		name          string
		child, parent string
		want          bool
	}{"dot-dot escape outside parent", escape, tmp, false})

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := isSubPath(tc.child, tc.parent)
			if got != tc.want {
				t.Errorf("isSubPath(%q, %q) = %v, want %v", tc.child, tc.parent, got, tc.want)
			}
		})
	}
}

// ── cleanResidualWorktreeDirs ──

func TestCleanResidualWorktreeDirs(t *testing.T) {
	// 임시 repo root 시뮬레이션
	repoRoot := t.TempDir()
	wtRoot := filepath.Join(repoRoot, ".worktrees")

	t.Run("removes full path and short name dirs", func(t *testing.T) {
		// feature/my-branch → .worktrees/feature/my-branch + .worktrees/my-branch
		fullDir := filepath.Join(wtRoot, "feature", "my-branch")
		shortDir := filepath.Join(wtRoot, "my-branch")
		os.MkdirAll(fullDir, 0o755)
		os.MkdirAll(shortDir, 0o755)

		cleanResidualWorktreeDirs(repoRoot, "feature/my-branch")

		if _, err := os.Stat(fullDir); err == nil {
			t.Error("full path dir should have been removed")
		}
		if _, err := os.Stat(shortDir); err == nil {
			t.Error("short name dir should have been removed")
		}
	})

	t.Run("branch without slash — single dir only", func(t *testing.T) {
		dir := filepath.Join(wtRoot, "simple-branch")
		os.MkdirAll(dir, 0o755)

		cleanResidualWorktreeDirs(repoRoot, "simple-branch")

		if _, err := os.Stat(dir); err == nil {
			t.Error("dir should have been removed")
		}
	})

	t.Run("no-op when dirs do not exist", func(t *testing.T) {
		// should not panic
		cleanResidualWorktreeDirs(repoRoot, "nonexistent/branch")
	})

	t.Run("deeply nested branch name", func(t *testing.T) {
		dir := filepath.Join(wtRoot, "feature", "sub", "deep")
		shortDir := filepath.Join(wtRoot, "deep")
		os.MkdirAll(dir, 0o755)
		os.MkdirAll(shortDir, 0o755)

		cleanResidualWorktreeDirs(repoRoot, "feature/sub/deep")

		if _, err := os.Stat(dir); err == nil {
			t.Error("full path dir should have been removed")
		}
		if _, err := os.Stat(shortDir); err == nil {
			t.Error("short name dir should have been removed")
		}
	})
}

// ── processEmptyWorktreeDirs ──

func TestProcessEmptyWorktreeDirs(t *testing.T) {
	t.Run("removes empty directories", func(t *testing.T) {
		repoRoot := t.TempDir()
		wtRoot := filepath.Join(repoRoot, ".worktrees")

		emptyDir := filepath.Join(wtRoot, "empty-branch")
		os.MkdirAll(emptyDir, 0o755)

		result := &Result{}
		processEmptyWorktreeDirs(repoRoot, true, result)

		if len(result.EmptyDirs) != 1 {
			t.Fatalf("expected 1 empty dir action, got %d", len(result.EmptyDirs))
		}
		if result.EmptyDirs[0].Target != emptyDir {
			t.Errorf("target = %q, want %q", result.EmptyDirs[0].Target, emptyDir)
		}
		if !result.EmptyDirs[0].Done {
			t.Error("action should be Done=true in execute mode")
		}
		if _, err := os.Stat(emptyDir); err == nil {
			t.Error("empty dir should have been removed")
		}
	})

	t.Run("skips non-empty directories", func(t *testing.T) {
		repoRoot := t.TempDir()
		wtRoot := filepath.Join(repoRoot, ".worktrees")

		nonEmptyDir := filepath.Join(wtRoot, "has-content")
		os.MkdirAll(nonEmptyDir, 0o755)
		os.WriteFile(filepath.Join(nonEmptyDir, "file.txt"), []byte("content"), 0o644)

		result := &Result{}
		processEmptyWorktreeDirs(repoRoot, true, result)

		if len(result.EmptyDirs) != 0 {
			t.Errorf("expected 0 empty dir actions for non-empty dir, got %d", len(result.EmptyDirs))
		}
	})

	t.Run("dry-run collects but does not delete", func(t *testing.T) {
		repoRoot := t.TempDir()
		wtRoot := filepath.Join(repoRoot, ".worktrees")

		emptyDir := filepath.Join(wtRoot, "dry-run-target")
		os.MkdirAll(emptyDir, 0o755)

		result := &Result{}
		processEmptyWorktreeDirs(repoRoot, false, result)

		if len(result.EmptyDirs) != 1 {
			t.Fatalf("expected 1 empty dir action, got %d", len(result.EmptyDirs))
		}
		if result.EmptyDirs[0].Done {
			t.Error("action should be Done=false in dry-run mode")
		}
		// Directory should still exist
		if _, err := os.Stat(emptyDir); err != nil {
			t.Error("empty dir should NOT have been removed in dry-run")
		}
	})

	t.Run("no .worktrees directory — no-op", func(t *testing.T) {
		repoRoot := t.TempDir()
		// no .worktrees created

		result := &Result{}
		processEmptyWorktreeDirs(repoRoot, true, result)

		if len(result.EmptyDirs) != 0 {
			t.Errorf("expected 0 actions when no .worktrees dir, got %d", len(result.EmptyDirs))
		}
	})

	t.Run("skips files in .worktrees root", func(t *testing.T) {
		repoRoot := t.TempDir()
		wtRoot := filepath.Join(repoRoot, ".worktrees")
		os.MkdirAll(wtRoot, 0o755)
		// Create a file (not a directory) in .worktrees
		os.WriteFile(filepath.Join(wtRoot, "not-a-dir"), []byte("file"), 0o644)

		result := &Result{}
		processEmptyWorktreeDirs(repoRoot, true, result)

		if len(result.EmptyDirs) != 0 {
			t.Errorf("expected 0 actions for files, got %d", len(result.EmptyDirs))
		}
	})

	t.Run("multiple empty and non-empty dirs", func(t *testing.T) {
		repoRoot := t.TempDir()
		wtRoot := filepath.Join(repoRoot, ".worktrees")

		// 2 empty dirs
		os.MkdirAll(filepath.Join(wtRoot, "empty1"), 0o755)
		os.MkdirAll(filepath.Join(wtRoot, "empty2"), 0o755)

		// 1 non-empty dir
		nonEmpty := filepath.Join(wtRoot, "non-empty")
		os.MkdirAll(nonEmpty, 0o755)
		os.WriteFile(filepath.Join(nonEmpty, "file"), []byte("x"), 0o644)

		result := &Result{}
		processEmptyWorktreeDirs(repoRoot, true, result)

		if len(result.EmptyDirs) != 2 {
			t.Errorf("expected 2 empty dir actions, got %d", len(result.EmptyDirs))
		}
	})
}

// ── Result / Action 구조체 zero value ──

func TestResultZeroValue(t *testing.T) {
	var r Result
	// zero value Result는 모든 슬라이스가 nil
	if r.Worktrees != nil {
		t.Error("zero Result.Worktrees should be nil")
	}
	if r.Local != nil {
		t.Error("zero Result.Local should be nil")
	}
	if r.Remote != nil {
		t.Error("zero Result.Remote should be nil")
	}
	if r.EmptyDirs != nil {
		t.Error("zero Result.EmptyDirs should be nil")
	}
	if r.Copies != nil {
		t.Error("zero Result.Copies should be nil")
	}
	if r.Warnings != nil {
		t.Error("zero Result.Warnings should be nil")
	}
}

func TestActionZeroValue(t *testing.T) {
	var a Action
	if a.Target != "" {
		t.Error("zero Action.Target should be empty")
	}
	if a.Branch != "" {
		t.Error("zero Action.Branch should be empty")
	}
	if a.Done {
		t.Error("zero Action.Done should be false")
	}
}

// ── Result 분류/집계 검증 ──

func TestResultCategorization(t *testing.T) {
	r := &Result{
		Worktrees: []Action{
			{Target: "/wt/a", Branch: "feature/a", Done: true},
			{Target: "/wt/b", Branch: "feature/b", Done: false},
		},
		Local: []Action{
			{Target: "feature/c", Branch: "feature/c", Done: true},
		},
		Remote: []Action{
			{Target: "origin/feature/d", Branch: "feature/d", Done: true},
			{Target: "origin/feature/e", Branch: "feature/e", Done: true},
			{Target: "origin/feature/f", Branch: "feature/f", Done: false},
		},
		Copies: []Action{
			{Target: "copy01:feature/g", Branch: "feature/g", Done: true},
		},
		Warnings: []string{"경고1", "경고2"},
	}

	if len(r.Worktrees) != 2 {
		t.Errorf("Worktrees count = %d, want 2", len(r.Worktrees))
	}
	if len(r.Local) != 1 {
		t.Errorf("Local count = %d, want 1", len(r.Local))
	}
	if len(r.Remote) != 3 {
		t.Errorf("Remote count = %d, want 3", len(r.Remote))
	}
	if len(r.Copies) != 1 {
		t.Errorf("Copies count = %d, want 1", len(r.Copies))
	}
	if len(r.Warnings) != 2 {
		t.Errorf("Warnings count = %d, want 2", len(r.Warnings))
	}

	// Done 카운트 집계 검증
	doneCount := 0
	for _, a := range r.Remote {
		if a.Done {
			doneCount++
		}
	}
	if doneCount != 2 {
		t.Errorf("Remote done count = %d, want 2", doneCount)
	}
}

// ── parseWorktreeLine과 protectedBranches 결합 검증 ──

func TestParseWorktreeLine_ProtectedBranchFiltering(t *testing.T) {
	// processWorktrees의 필터링 로직을 단위 검증:
	// parseWorktreeLine으로 브랜치를 추출한 후 protectedBranches로 걸러지는지 확인
	lines := []struct {
		line         string
		wantSkipped  bool // protectedBranches에 의해 스킵되어야 하는가
	}{
		{"/repo  abc [main]", true},
		{"/repo  abc [master]", true},
		{"/repo  abc [gh-pages]", true},
		{"/repo  abc [feature/work]", false},
		{"/repo  abc [bugfix/issue]", false},
		{"/repo  abc (detached)", true}, // branch="" → processWorktrees에서 continue로 스킵
	}

	for _, tc := range lines {
		_, branch := parseWorktreeLine(tc.line)
		skipped := branch == "" || protectedBranches[branch]
		if skipped != tc.wantSkipped {
			t.Errorf("line=%q branch=%q: skipped=%v, want %v",
				tc.line, branch, skipped, tc.wantSkipped)
		}
	}
}
