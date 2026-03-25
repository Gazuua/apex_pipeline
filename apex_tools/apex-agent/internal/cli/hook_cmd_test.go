// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import "testing"

func TestContainsShellCommand_GhPrMerge(t *testing.T) {
	if !containsShellCommand("gh pr merge --squash", "gh pr merge") {
		t.Error("expected true for direct gh pr merge command")
	}
}

func TestContainsShellCommand_NotInQuotes(t *testing.T) {
	// "gh pr merge" appears as part of a git commit message, not as a real command
	if containsShellCommand(`git commit -m 'gh pr merge'`, "gh pr merge") {
		t.Error("expected false when gh pr merge is inside a quoted commit message")
	}
}

func TestContainsShellCommand_ChainedCommand(t *testing.T) {
	if !containsShellCommand("echo ok && gh pr merge", "gh pr merge") {
		t.Error("expected true for chained command containing gh pr merge")
	}
}

func TestFindShellSubcommand_GhPrCreate(t *testing.T) {
	got := findShellSubcommand("gh pr create --base main", "gh pr create")
	if got == "" {
		t.Fatal("expected non-empty subcommand, got empty")
	}
	if got != "gh pr create --base main" {
		t.Errorf("expected %q, got %q", "gh pr create --base main", got)
	}
}

func TestExtractFlag_Base(t *testing.T) {
	got := extractFlag("gh pr create --base main", "--base")
	if got != "main" {
		t.Errorf("expected %q, got %q", "main", got)
	}
}

func TestExtractFlag_BaseEquals(t *testing.T) {
	got := extractFlag("gh pr create --base=dev", "--base")
	if got != "dev" {
		t.Errorf("expected %q, got %q", "dev", got)
	}
}

func TestExtractFlag_Missing(t *testing.T) {
	got := extractFlag("gh pr create", "--base")
	if got != "" {
		t.Errorf("expected empty string, got %q", got)
	}
}
