// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"fmt"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("hook")

// blockedBuildPatterns are regex patterns that indicate direct build tool invocation.
var blockedBuildPatterns = []*regexp.Regexp{
	regexp.MustCompile(`cmake --build`),
	regexp.MustCompile(`cmake --preset`),
	regexp.MustCompile(`\bninja\b`),
	regexp.MustCompile(`\bmsbuild\b`),
	regexp.MustCompile(`\bcl\.exe\b`),
	regexp.MustCompile(`(^|[;&|]\s*)build\.bat`),
	regexp.MustCompile(`cmd\.exe.*build\.bat`),
	regexp.MustCompile(`\bbench_\w+`),
}

// readOnlyPrefixes are command prefixes that are always allowed (read-only operations).
var readOnlyPrefixes = []string{
	"cat", "head", "tail", "grep", "less", "more", "type", "echo", "ls", "dir", "file", "wc", "read",
}

// chainSeparatorRe splits shell commands by chain operators (&&, ||, ;, |).
// Pipe (|) is included because `echo apex-agent | ninja` should not bypass the gate.
var chainSeparatorRe = regexp.MustCompile(`\s*(\&\&|\|\||\||\;)\s*`)

// splitChainedCommands splits a shell command string by chain operators.
// Returns individual sub-commands. A single (non-chained) command returns a
// one-element slice.
func splitChainedCommands(command string) []string {
	parts := chainSeparatorRe.Split(command, -1)
	var result []string
	for _, p := range parts {
		trimmed := strings.TrimSpace(p)
		if trimmed != "" {
			result = append(result, trimmed)
		}
	}
	if len(result) == 0 {
		return []string{command}
	}
	return result
}

// commandSubstitutionRe detects shell command substitution patterns ($(...), `...`, <(...)).
// Commands containing these are not eligible for whitelist bypass because the inner
// commands could execute blocked tools (e.g., `echo $(ninja -C out)`).
var commandSubstitutionRe = regexp.MustCompile(`\$\(|` + "`" + `|<\(`)

// hasCommandSubstitution returns true if the command contains shell command substitution.
func hasCommandSubstitution(command string) bool {
	return commandSubstitutionRe.MatchString(command)
}

// isAllowedSubCommand checks whether a single (non-chained) sub-command is on
// the whitelist. Returns true if it should bypass blocked-pattern checks.
// Commands containing command substitution ($(...), backticks, <(...)) are never whitelisted.
func isAllowedSubCommand(sub string) bool {
	// Command substitution can embed blocked commands — never whitelist.
	if hasCommandSubstitution(sub) {
		return false
	}

	// Check token basenames for approved wrappers.
	// This handles `bash ./path/to/run-hook ...` and `apex-agent ...` equally.
	trimmed := strings.TrimSpace(sub)
	tokens := strings.Fields(trimmed)
	if len(tokens) == 0 {
		return false
	}

	// Approved wrappers — any token's basename matches approved name.
	approvedWrappers := []string{"queue-lock.sh", "apex-agent", "run-hook"}
	for _, tok := range tokens {
		base := filepath.Base(tok)
		for _, aw := range approvedWrappers {
			if base == aw {
				return true
			}
		}
	}
	// GitHub CLI — first token must be "gh".
	if tokens[0] == "gh" && len(tokens) >= 2 && (tokens[1] == "pr" || tokens[1] == "run") {
		return true
	}

	// Go toolchain.
	if hasGoToolCommand(sub) {
		return true
	}

	// Read-only commands — match on first token only.
	for _, prefix := range readOnlyPrefixes {
		if tokens[0] == prefix {
			return true
		}
	}

	return false
}

// ValidateBuild checks if a command is a direct build tool invocation.
// Returns nil if allowed, error with reason if blocked.
//
// For chained commands (&&, ||, ;, |), each sub-command is validated
// independently. If ANY sub-command is blocked, the entire command is rejected.
// Whitelist patterns are also checked per sub-command, so
// `echo apex-agent && ninja build` is correctly blocked.
func ValidateBuild(command string) error {
	if command == "" {
		return nil
	}

	subCommands := splitChainedCommands(command)

	// Fast path: single command with no chain operators — preserve original
	// behaviour where a whitelisted command bypasses all checks.
	if len(subCommands) == 1 {
		if isAllowedSubCommand(subCommands[0]) {
			return nil
		}
		return validateSingleCommand(subCommands[0])
	}

	// Chained command: each sub-command must pass independently.
	for _, sub := range subCommands {
		if isAllowedSubCommand(sub) {
			continue
		}
		if err := validateSingleCommand(sub); err != nil {
			return err
		}
	}

	return nil
}

// validateSingleCommand checks a single (non-chained) command against blocked
// patterns. It is called only after whitelist checks have already been applied.
func validateSingleCommand(command string) error {
	// Check git branch creation commands.
	if isBlockedGitBranch(command) {
		ml.Warn("git branch creation blocked", "command", command)
		return fmt.Errorf("차단: 직접 브랜치 생성 금지. 'apex-agent handoff notify start --branch-name <name>' 을 사용하세요")
	}

	// Check blocked patterns.
	for _, pat := range blockedBuildPatterns {
		if pat.MatchString(command) {
			ml.Warn("build command blocked", "pattern", pat.String())
			return fmt.Errorf("차단: 빌드/벤치마크는 apex-agent queue build|benchmark를 통해서만 실행할 수 있습니다. (matched: %s)", pat.String())
		}
	}

	return nil
}

// blockedGitBranchPatterns detect git branch creation commands.
var blockedGitBranchPatterns = []*regexp.Regexp{
	regexp.MustCompile(`\bgit\s+checkout\s+-[bB]\b`),
	regexp.MustCompile(`\bgit\s+switch\s+(-c|--create)\b`),
	regexp.MustCompile(`\bgit\s+worktree\s+add\s+.*-b\b`),
}

// isBlockedGitBranch returns true if the command creates a git branch.
func isBlockedGitBranch(command string) bool {
	for _, pat := range blockedGitBranchPatterns {
		if pat.MatchString(command) {
			return true
		}
	}
	return isGitBranchCreate(command)
}

// branchMutationFlags are git branch flags that create or rename branches.
var branchMutationFlags = map[string]bool{
	"-m": true, "-M": true, // rename
	"-c": true, "-C": true, // copy
	"--move": true, "--copy": true,
}

// branchSafeFlags are git branch flags that indicate query or delete operations.
// When these are present, non-flag arguments are operands (not new branch names).
var branchSafeFlags = map[string]bool{
	"-d": true, "-D": true, "--delete": true, // delete
	"-a": true, "--all": true, // list all
	"-v": true, "-vv": true, "--verbose": true, // verbose
	"-l": true, "--list": true, // list
	"-r": true, "--remotes": true, // remote list
	"--merged": true, "--no-merged": true, // filter
	"--contains": true, "--no-contains": true, // filter
	"--sort": true, "--format": true, // sort/format
}

// isGitBranchCreate detects `git branch <name>` (creation, not query/delete).
// Also blocks rename (-m/-M) and copy (-c/-C) which create new branch refs.
// Allowed: git branch, git branch -a, git branch -D foo, git branch -d foo, git branch -v, git branch --list
// Blocked: git branch feature/foo, git branch -m old new, git branch -c src dst
func isGitBranchCreate(command string) bool {
	fields := strings.Fields(command)
	for i := 0; i < len(fields)-1; i++ {
		if fields[i] != "git" || fields[i+1] != "branch" {
			continue
		}
		// Found "git branch" — scan all remaining args
		remaining := fields[i+2:]
		hasNonFlag := false
		hasMutationFlag := false
		hasSafeFlag := false
		for _, arg := range remaining {
			if strings.HasPrefix(arg, "-") {
				if branchMutationFlags[arg] {
					hasMutationFlag = true
				}
				if branchSafeFlags[arg] {
					hasSafeFlag = true
				}
			} else {
				hasNonFlag = true
			}
		}
		// Mutation flags (rename/copy) always create a new ref
		if hasMutationFlag {
			return true
		}
		// Safe flags (delete/query) with non-flag args → not creation
		if hasSafeFlag {
			return false
		}
		// Non-flag argument with no flags → branch creation
		return hasNonFlag
	}
	return false
}

// goToolRe matches word-boundary-safe Go toolchain sub-commands.
// Excludes `go generate` (can execute arbitrary commands) and `go clean`/`go env -w`.
var goToolRe = regexp.MustCompile(`\bgo\s+(build|test|run|install|vet|fmt|mod|get|work)\b`)

// hasGoToolCommand checks if the command contains a safe Go toolchain invocation.
// Uses word-boundary matching to prevent false positives like "mango build".
func hasGoToolCommand(command string) bool {
	return goToolRe.MatchString(command)
}

// ValidateBacklog blocks direct access to backlog files (Edit, Write, and Read).
// All backlog access must go through CLI (backlog list/show/add/update/resolve/export).
func ValidateBacklog(filePath string) error {
	normalized := strings.ToUpper(strings.ReplaceAll(filePath, "\\", "/"))
	if strings.HasSuffix(normalized, "/DOCS/BACKLOG.MD") ||
		strings.HasSuffix(normalized, "/DOCS/BACKLOG_HISTORY.MD") ||
		strings.HasSuffix(normalized, "/DOCS/BACKLOG.JSON") {
		return fmt.Errorf("차단: BACKLOG 파일 직접 접근 금지.\n" +
			"  조회: backlog list / backlog show <ID>\n" +
			"  수정: backlog add/update/resolve/release/export CLI를 사용하세요")
	}
	return nil
}

// ValidateMerge is now handled by CLI layer via daemon IPC (hook_cmd.go).
// The file-based lock check was removed — queue module uses DB-based locking.
