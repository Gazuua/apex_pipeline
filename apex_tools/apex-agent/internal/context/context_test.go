// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package context

import (
	"strings"
	"testing"
)

func TestGenerate_ContainsHeader(t *testing.T) {
	// Use the current repo root (project root is 2 levels up from this package).
	// We pass "." which goes through os.Getwd() in practice, but for the test
	// we use a known valid directory — the temp dir which always exists.
	t.TempDir() // ensure a temp dir is available

	// Run Generate with a non-existent path — git commands will fail gracefully.
	out := Generate(t.TempDir())
	if !strings.Contains(out, "=== Project Context (auto-injected) ===") {
		t.Errorf("expected header not found in output:\n%s", out)
	}
	if !strings.Contains(out, "=== End Project Context ===") {
		t.Errorf("expected footer not found in output:\n%s", out)
	}
}

func TestGenerate_ContainsGitSection(t *testing.T) {
	out := Generate(t.TempDir())
	if !strings.Contains(out, "--- Git Status ---") {
		t.Errorf("expected Git Status section not found:\n%s", out)
	}
}

func TestWorkspaceID(t *testing.T) {
	tests := []struct {
		root string
		want string
	}{
		{"/path/to/apex_pipeline_branch_02", "branch_02"},
		{"/path/to/apex_pipeline_main", "main"},
		{"/path/to/other_project", "other_project"},
	}
	for _, tt := range tests {
		got := workspaceID(tt.root)
		if got != tt.want {
			t.Errorf("workspaceID(%q) = %q, want %q", tt.root, got, tt.want)
		}
	}
}
