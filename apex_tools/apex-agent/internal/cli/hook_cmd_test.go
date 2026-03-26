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

// --- Additional edge-case tests (BACKLOG-223) ---

func TestExtractFlag_Table(t *testing.T) {
	tests := []struct {
		name    string
		command string
		flag    string
		want    string
	}{
		{name: "flag at end without value", command: "gh pr create --base", flag: "--base", want: ""},
		{name: "flag=value no spaces", command: "--target=apex_core", flag: "--target", want: "apex_core"},
		{name: "multiple flags", command: "cmd --foo bar --base main --baz", flag: "--base", want: "main"},
		{name: "flag with equals and trailing args", command: "cmd --base=dev --squash", flag: "--base", want: "dev"},
		{name: "similar flag prefix", command: "cmd --base-ref main", flag: "--base", want: ""},
		{name: "empty command", command: "", flag: "--base", want: ""},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := extractFlag(tt.command, tt.flag)
			if got != tt.want {
				t.Errorf("extractFlag(%q, %q) = %q, want %q", tt.command, tt.flag, got, tt.want)
			}
		})
	}
}

func TestFindShellSubcommand_Table(t *testing.T) {
	tests := []struct {
		name    string
		command string
		prefix  string
		want    string
	}{
		{name: "exact match", command: "gh pr create", prefix: "gh pr create", want: "gh pr create"},
		{name: "no match", command: "git push origin", prefix: "gh pr create", want: ""},
		{name: "semicolon chain", command: "echo ok; gh pr create --base main", prefix: "gh pr create", want: "gh pr create --base main"},
		{name: "pipe chain", command: "echo ok || gh pr create", prefix: "gh pr create", want: "gh pr create"},
		{name: "triple chain", command: "a && b && gh pr create --title t", prefix: "gh pr create", want: "gh pr create --title t"},
		{name: "prefix in middle of word", command: "gh pr create-draft", prefix: "gh pr create", want: "gh pr create-draft"},
		{name: "empty command", command: "", prefix: "gh pr", want: ""},
		{name: "whitespace padding", command: "  gh pr create  ", prefix: "gh pr create", want: "gh pr create"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := findShellSubcommand(tt.command, tt.prefix)
			if got != tt.want {
				t.Errorf("findShellSubcommand(%q, %q) = %q, want %q", tt.command, tt.prefix, got, tt.want)
			}
		})
	}
}

func TestContainsShellCommand_Table(t *testing.T) {
	tests := []struct {
		name    string
		command string
		prefix  string
		want    bool
	}{
		{name: "simple match", command: "gh pr merge --squash", prefix: "gh pr merge", want: true},
		{name: "no match", command: "git status", prefix: "gh pr merge", want: false},
		{name: "empty command", command: "", prefix: "gh pr merge", want: false},
		{name: "empty prefix", command: "anything", prefix: "", want: true},
		{name: "chained &&", command: "echo hi && gh pr merge", prefix: "gh pr merge", want: true},
		{name: "chained ||", command: "echo hi || gh pr merge", prefix: "gh pr merge", want: true},
		{name: "chained ;", command: "echo hi; gh pr merge", prefix: "gh pr merge", want: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := containsShellCommand(tt.command, tt.prefix)
			if got != tt.want {
				t.Errorf("containsShellCommand(%q, %q) = %v, want %v", tt.command, tt.prefix, got, tt.want)
			}
		})
	}
}
