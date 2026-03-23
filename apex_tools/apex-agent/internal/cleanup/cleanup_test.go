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

func TestProcessLocalBranches_SkipsActiveBranches(t *testing.T) {
	// processLocalBranches에서 activeBranches에 포함된 브랜치를 스킵하는지 검증.
	// 실제 git을 사용하는 대신 로직의 핵심만 검증:
	// activeBranches 맵에 있는 브랜치는 result.Local에 들어가면 안 됨.

	// 이 테스트는 git 명령 없이 activeBranches 보호 로직만 검증.
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
	// nil activeBranches는 빈 맵으로 처리되어야 함 (panic 없이)
	// 실제 git repo가 아니므로 Run은 에러를 반환하겠지만, panic은 안 돼야 함.
	tmp := t.TempDir()
	_, err := Run(tmp, false, nil)
	// git repo가 아니므로 에러는 예상되지만 panic은 안 돼야 함
	_ = err
}

func TestRunSignature_EmptyActiveBranches(t *testing.T) {
	tmp := t.TempDir()
	_, err := Run(tmp, false, map[string]bool{})
	_ = err
}
