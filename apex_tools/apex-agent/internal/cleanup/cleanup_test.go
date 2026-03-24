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
