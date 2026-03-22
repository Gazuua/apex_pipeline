// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cleanup

import (
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
