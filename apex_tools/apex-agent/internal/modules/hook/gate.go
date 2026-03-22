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

	// Allow approved build/benchmark wrappers.
	if strings.Contains(command, "queue-lock.sh") ||
		strings.Contains(command, "apex-agent") ||
		strings.Contains(command, "run-hook") {
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

	// Check blocked patterns.
	for _, pat := range blockedBuildPatterns {
		if pat.MatchString(command) {
			ml.Warn("build command blocked", "pattern", pat.String())
			return fmt.Errorf("차단: 빌드/벤치마크는 apex-agent queue build|benchmark를 통해서만 실행할 수 있습니다. (matched: %s)", pat.String())
		}
	}

	return nil
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

// ValidateMerge is now handled by CLI layer via daemon IPC (hook_cmd.go).
// The file-based lock check was removed — queue module uses DB-based locking.
