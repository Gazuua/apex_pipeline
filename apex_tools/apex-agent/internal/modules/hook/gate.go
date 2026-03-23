// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"fmt"
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

// ValidateBuild checks if a command is a direct build tool invocation.
// Returns nil if allowed, error with reason if blocked.
func ValidateBuild(command string) error {
	if command == "" {
		return nil
	}

	// Allow approved build/benchmark wrappers and GitHub CLI.
	if strings.Contains(command, "queue-lock.sh") ||
		strings.Contains(command, "apex-agent") ||
		strings.Contains(command, "run-hook") ||
		strings.Contains(command, "gh pr") ||
		strings.Contains(command, "gh run") {
		return nil
	}

	// Allow Go toolchain commands (not C++ build tools).
	// NOTE: strings.Contains 방식은 체인 명령(&&, ||, ;)에서 우회 가능하나,
	// hook 컨텍스트에서 오탐 위험이 극히 낮으므로 허용.
	// first-token 보조 체크로 "go" 단독 실행도 허용.
	if hasGoToolCommand(command) {
		return nil
	}

	// Allow read-only commands.
	trimmed := strings.TrimSpace(command)
	for _, prefix := range readOnlyPrefixes {
		if strings.HasPrefix(trimmed, prefix+" ") || trimmed == prefix {
			return nil
		}
	}

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

// isGitBranchCreate detects `git branch <name>` (creation, not query).
// Allowed: git branch, git branch -a, git branch -D, git branch -d, git branch -v, git branch --list
// Blocked: git branch feature/foo (non-flag argument = branch creation)
func isGitBranchCreate(command string) bool {
	fields := strings.Fields(command)
	for i := 0; i < len(fields)-1; i++ {
		if fields[i] != "git" || fields[i+1] != "branch" {
			continue
		}
		// Found "git branch" — check remaining args
		for _, arg := range fields[i+2:] {
			if strings.HasPrefix(arg, "-") {
				return false // flag present → query or delete command
			}
			return true // non-flag argument → branch creation
		}
		return false // "git branch" with no args → list
	}
	return false
}

// hasGoToolCommand checks if the command contains a Go toolchain invocation.
// Uses both strings.Contains (for chained commands) and first-token matching.
func hasGoToolCommand(command string) bool {
	goSubCommands := []string{"go build", "go test", "go run", "go install"}
	for _, sub := range goSubCommands {
		if strings.Contains(command, sub) {
			return true
		}
	}
	// first-token check: 명령의 첫 토큰이 "go"이면 허용
	firstToken := strings.Fields(strings.TrimSpace(command))
	if len(firstToken) > 0 && firstToken[0] == "go" {
		return true
	}
	return false
}

// ValidateBacklog blocks direct editing of docs/BACKLOG.md and docs/BACKLOG_HISTORY.md.
// All backlog modifications must go through CLI (backlog add/update/resolve/release/export).
func ValidateBacklog(filePath string) error {
	normalized := strings.ReplaceAll(filePath, "\\", "/")
	if strings.HasSuffix(normalized, "/docs/BACKLOG.md") ||
		strings.HasSuffix(normalized, "/docs/BACKLOG_HISTORY.md") {
		return fmt.Errorf("차단: BACKLOG 파일 직접 편집 금지.\n" +
			"  backlog add/update/resolve/release/export CLI를 사용하세요")
	}
	return nil
}

// ValidateMerge is now handled by CLI layer via daemon IPC (hook_cmd.go).
// The file-based lock check was removed — queue module uses DB-based locking.
