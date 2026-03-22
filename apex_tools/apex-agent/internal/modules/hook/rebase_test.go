// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import "testing"

func TestIsGitPush(t *testing.T) {
	tests := []struct {
		cmd  string
		want bool
	}{
		{"git push", true},
		{"git push origin main", true},
		{"git push --force-with-lease", true},
		{"git status", false},
		{"gh pr create", false},
		// False positive cases — strings.Contains-based matching
		{"echo git push log", true},   // contains "git" and "push"
		{"git pusher utility", true},   // partial word match
		{"push git changes log", true}, // reversed order still matches
	}
	for _, tt := range tests {
		if got := isGitPush(tt.cmd); got != tt.want {
			t.Errorf("isGitPush(%q) = %v, want %v", tt.cmd, got, tt.want)
		}
	}
}

func TestIsGHPRCreate(t *testing.T) {
	tests := []struct {
		cmd  string
		want bool
	}{
		{"gh pr create --title test", true},
		{"gh pr merge", false},
		{"git push", false},
		// False positive cases — strings.Contains-based matching
		{"echo gh pr create log", true},        // contains all three tokens
		{"gh pr create-review --flag", true},    // partial word match
		{"create a gh pr description", true},    // reversed order still matches
		{"gh issue create --label pr", true},    // "gh", "pr", "create" scattered
	}
	for _, tt := range tests {
		if got := isGHPRCreate(tt.cmd); got != tt.want {
			t.Errorf("isGHPRCreate(%q) = %v, want %v", tt.cmd, got, tt.want)
		}
	}
}

func TestEnforceRebase_SkipsNonPush(t *testing.T) {
	msg, err := EnforceRebase("git status", "/tmp")
	if err != nil {
		t.Errorf("non-push should not error: %v", err)
	}
	if msg != "" {
		t.Errorf("non-push should return empty msg, got: %s", msg)
	}
}

func TestEnforceRebase_SkipsGitCommit(t *testing.T) {
	msg, err := EnforceRebase("git commit -m 'test'", "/tmp")
	if err != nil {
		t.Errorf("git commit should not error: %v", err)
	}
	if msg != "" {
		t.Errorf("git commit should return empty msg, got: %s", msg)
	}
}

func TestEnforceRebase_SkipsGHPRMerge(t *testing.T) {
	msg, err := EnforceRebase("gh pr merge --squash", "/tmp")
	if err != nil {
		t.Errorf("gh pr merge should not error: %v", err)
	}
	if msg != "" {
		t.Errorf("gh pr merge should return empty msg, got: %s", msg)
	}
}

func TestEnforceRebase_SkipsEmptyCommand(t *testing.T) {
	msg, err := EnforceRebase("", "/tmp")
	if err != nil {
		t.Errorf("empty command should not error: %v", err)
	}
	if msg != "" {
		t.Errorf("empty command should return empty msg, got: %s", msg)
	}
}
