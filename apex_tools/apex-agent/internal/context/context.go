// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package context

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// Generate produces the session context string for Claude.
// workspaceRoot is the absolute path to the project root.
func Generate(workspaceRoot string) string {
	var b strings.Builder

	b.WriteString("=== Project Context (auto-injected) ===\n")

	// Git status
	b.WriteString("\n--- Git Status ---\n")
	branch := gitCurrentBranch(workspaceRoot)
	fmt.Fprintf(&b, "Branch: %s\n", branch)

	// Auto-fetch main if on main branch
	if branch == "main" {
		b.WriteString("\nAuto-fetching latest main...\n")
		if out, _ := gitExec(workspaceRoot, "fetch", "origin", "main"); out != "" {
			for _, line := range strings.Split(out, "\n") {
				fmt.Fprintf(&b, "  %s\n", line)
			}
		}
		if out, _ := gitExec(workspaceRoot, "pull", "origin", "main"); out != "" {
			for _, line := range strings.Split(out, "\n") {
				fmt.Fprintf(&b, "  %s\n", line)
			}
		}
	}

	// Status + recent commits
	b.WriteString("\n")
	if out, _ := gitExec(workspaceRoot, "status", "--short"); out != "" {
		b.WriteString(out)
		b.WriteString("\n")
	}
	b.WriteString("Recent commits:\n")
	if out, _ := gitExec(workspaceRoot, "log", "--oneline", "-5"); out != "" {
		b.WriteString(out)
		b.WriteString("\n")
	}

	// Branch handoff status (non-main branches)
	if branch != "" && branch != "main" {
		b.WriteString("\n--- Branch Handoff ---\n")
		branchID := filepath.Base(workspaceRoot)
		branchID = strings.TrimPrefix(branchID, "apex_pipeline_")

		hDir := handoffDir()
		activeFile := filepath.Join(hDir, "active", branchID+".yml")

		if _, err := os.Stat(activeFile); os.IsNotExist(err) {
			fmt.Fprintf(&b, "WARNING [BLOCKED]: 이 워크스페이스(%s)가 핸드오프 시스템에 미등록 상태!\n", branchID)
			b.WriteString("  모든 Edit/Write/git commit이 차단됩니다.\n")
			b.WriteString("  → 즉시 실행: apex-agent handoff notify start --scopes <s> --summary \"설명\" [--backlog <N>]\n")
			b.WriteString("  (설계 불필요 시: apex-agent handoff notify start --skip-design --scopes <s> --summary \"설명\")\n")
		} else {
			fmt.Fprintf(&b, "Registered: %s\n", branchID)
			if data, err := os.ReadFile(activeFile); err == nil {
				lines := strings.Split(string(data), "\n")
				for _, l := range lines {
					if strings.HasPrefix(l, "backlog:") {
						fmt.Fprintf(&b, "  %s\n", l)
						break
					}
				}
				status := ""
				for _, l := range lines {
					if strings.HasPrefix(l, "status:") {
						status = strings.TrimSpace(strings.TrimPrefix(l, "status:"))
						break
					}
				}
				fmt.Fprintf(&b, "  status: %s\n", status)
				switch status {
				case "started":
					b.WriteString("  → 다음: notify design (설계 완료 시) 또는 notify start --skip-design (설계 불필요 시)\n")
				case "design-notified":
					b.WriteString("  → 다음: notify plan (구현 계획 완료 시)\n")
				case "implementing":
					b.WriteString("  → 구현 진행 중 (소스 편집 허용)\n")
				}
			}
		}
	}

	// Other active branches
	hDir := handoffDir()
	activeDir := filepath.Join(hDir, "active")
	if entries, err := os.ReadDir(activeDir); err == nil {
		myBranchID := filepath.Base(workspaceRoot)
		myBranchID = strings.TrimPrefix(myBranchID, "apex_pipeline_")

		var activeLines []string
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".yml") {
				continue
			}
			data, err := os.ReadFile(filepath.Join(activeDir, e.Name()))
			if err != nil {
				continue
			}
			lines := strings.Split(string(data), "\n")
			br := fieldValue(lines, "branch:")
			if br == myBranchID {
				continue
			}
			status := fieldValue(lines, "status:")
			backlog := fieldValue(lines, "backlog:")
			summary := strings.Trim(fieldValue(lines, "summary:"), `"`)

			// scopes: list under "scopes:" key
			var scopes []string
			inScopes := false
			for _, l := range lines {
				trimmed := strings.TrimSpace(l)
				if trimmed == "scopes:" {
					inScopes = true
					continue
				}
				if inScopes {
					if strings.HasPrefix(trimmed, "- ") {
						scopes = append(scopes, strings.TrimPrefix(trimmed, "- "))
					} else if trimmed != "" {
						inScopes = false
					}
				}
			}

			blLabel := ""
			if backlog != "" {
				blLabel = "BACKLOG-" + backlog + " "
			}
			scopeLabel := ""
			if len(scopes) > 0 {
				scopeLabel = " (" + strings.Join(scopes, ",") + ")"
			}
			if summary == "" {
				summary = "설명 없음"
			}
			statusStr := status
			if statusStr == "" {
				statusStr = "unknown"
			}
			activeLines = append(activeLines, fmt.Sprintf("  %s [%s]%s %s— %s", br, statusStr, scopeLabel, blLabel, summary))
		}
		if len(activeLines) > 0 {
			fmt.Fprintf(&b, "\n--- Other Active Branches (%d개 진행 중) ---\n", len(activeLines))
			for _, l := range activeLines {
				b.WriteString(l)
				b.WriteString("\n")
			}
		}
	}

	// Handoff storage location
	b.WriteString("\n--- Handoff Storage ---\n")
	fmt.Fprintf(&b, "Path: %s\n", hDir)

	b.WriteString("=== End Project Context ===\n")
	return b.String()
}

// handoffDir returns the platform-specific handoff storage directory.
func handoffDir() string {
	// Allow override via environment variable
	if override := os.Getenv("APEX_HANDOFF_DIR"); override != "" {
		return override
	}
	if localAppData := os.Getenv("LOCALAPPDATA"); localAppData != "" {
		return filepath.Join(localAppData, "apex-branch-handoff")
	}
	if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
		return filepath.Join(xdg, "apex-branch-handoff")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "apex-branch-handoff")
}

// gitCurrentBranch returns the current git branch name.
func gitCurrentBranch(root string) string {
	out, err := exec.Command("git", "-C", root, "branch", "--show-current").Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

// gitExec runs a git command in the given root and returns combined output.
func gitExec(root string, args ...string) (string, error) {
	cmdArgs := append([]string{"-C", root}, args...)
	out, err := exec.Command("git", cmdArgs...).CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

// fieldValue extracts the value from a YAML line matching "key: value".
func fieldValue(lines []string, prefix string) string {
	for _, l := range lines {
		trimmed := strings.TrimSpace(l)
		if strings.HasPrefix(trimmed, prefix) {
			return strings.TrimSpace(strings.TrimPrefix(trimmed, prefix))
		}
	}
	return ""
}
