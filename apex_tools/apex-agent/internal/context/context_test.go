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

func TestGenerate_ContainsHandoffStorage(t *testing.T) {
	out := Generate(t.TempDir())
	if !strings.Contains(out, "--- Handoff Storage ---") {
		t.Errorf("expected Handoff Storage section not found:\n%s", out)
	}
}

func TestHandoffDir_UsesLocalAppData(t *testing.T) {
	t.Setenv("APEX_HANDOFF_DIR", "")
	t.Setenv("LOCALAPPDATA", "/tmp/testlocal")
	t.Setenv("XDG_DATA_HOME", "")
	dir := handoffDir()
	if !strings.Contains(dir, "apex-branch-handoff") {
		t.Errorf("expected apex-branch-handoff in dir, got: %s", dir)
	}
}

func TestHandoffDir_UsesOverride(t *testing.T) {
	t.Setenv("APEX_HANDOFF_DIR", "/custom/handoff")
	dir := handoffDir()
	if dir != "/custom/handoff" {
		t.Errorf("expected /custom/handoff, got: %s", dir)
	}
}

func TestFieldValue(t *testing.T) {
	lines := []string{
		"branch: feature/test",
		"status: implementing",
		"backlog: 42",
		`summary: "some description"`,
	}
	tests := []struct {
		prefix string
		want   string
	}{
		{"branch:", "feature/test"},
		{"status:", "implementing"},
		{"backlog:", "42"},
		{"summary:", `"some description"`},
		{"missing:", ""},
	}
	for _, tt := range tests {
		got := fieldValue(lines, tt.prefix)
		if got != tt.want {
			t.Errorf("fieldValue(%q) = %q, want %q", tt.prefix, got, tt.want)
		}
	}
}
