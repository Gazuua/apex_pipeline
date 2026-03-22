// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
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

// ValidateMerge checks if a merge lock is held before allowing gh pr merge.
// Returns nil if allowed, error with reason if blocked.
// Uses file-based lock check (Phase 4 will migrate to DB-based queue).
func ValidateMerge(command, cwd string) error {
	if command == "" {
		return nil
	}

	// Only intercept gh pr merge commands.
	if !strings.Contains(command, "gh pr merge") {
		return nil
	}

	// Determine queue directory.
	queueDir := queueDirectory()

	// Check if merge lock exists.
	lockDir := filepath.Join(queueDir, "merge.lock")
	if _, err := os.Stat(lockDir); os.IsNotExist(err) {
		ml.Warn("merge blocked: no lock held")
		return fmt.Errorf("차단: 먼저 apex-agent queue merge acquire를 실행하세요.")
	}

	// Check owner branch matches.
	ownerFile := filepath.Join(queueDir, "merge.owner")
	data, err := os.ReadFile(ownerFile)
	if err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "BRANCH=") {
				ownerBranch := strings.TrimPrefix(line, "BRANCH=")
				if ownerBranch != "" && !strings.Contains(cwd, ownerBranch) {
					return fmt.Errorf("차단: merge lock 소유자가 %s입니다 (현재: %s). 먼저 apex-agent queue merge acquire를 실행하세요.", ownerBranch, cwd)
				}
			}
		}
	}

	return nil
}

// queueDirectory returns the platform-specific queue directory path.
// Checks APEX_BUILD_QUEUE_DIR env var first for testability.
func queueDirectory() string {
	if dir := os.Getenv("APEX_BUILD_QUEUE_DIR"); dir != "" {
		return dir
	}
	if runtime.GOOS == "windows" {
		if localAppData := os.Getenv("LOCALAPPDATA"); localAppData != "" {
			return filepath.Join(localAppData, "apex-build-queue")
		}
	}
	if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
		return filepath.Join(xdg, "apex-build-queue")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "apex-build-queue")
}
