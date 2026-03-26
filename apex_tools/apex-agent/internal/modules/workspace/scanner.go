// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workspace

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// ScanEntry represents a discovered workspace directory.
type ScanEntry struct {
	WorkspaceID string
	Directory   string
	GitBranch   string
	GitStatus   string // CLEAN, DIRTY, UNKNOWN
}

// ScanDirectories scans root for directories starting with repoName prefix,
// checks git status for each, and returns scan entries.
func ScanDirectories(root, repoName string) ([]ScanEntry, error) {
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil, fmt.Errorf("read workspace root %s: %w", root, err)
	}

	var result []ScanEntry
	for _, e := range entries {
		if !e.IsDir() || !strings.HasPrefix(e.Name(), repoName) {
			continue
		}
		dir := filepath.Join(root, e.Name())

		if _, err := os.Stat(filepath.Join(dir, ".git")); err != nil {
			continue
		}

		wsID := extractWorkspaceID(e.Name(), repoName)
		branch := gitCurrentBranch(dir)
		status := gitStatus(dir)

		result = append(result, ScanEntry{
			WorkspaceID: wsID,
			Directory:   dir,
			GitBranch:   branch,
			GitStatus:   status,
		})
	}
	return result, nil
}

func extractWorkspaceID(dirName, repoName string) string {
	trimmed := strings.TrimPrefix(dirName, repoName+"_")
	if trimmed == dirName {
		return dirName
	}
	return trimmed
}

func gitCurrentBranch(dir string) string {
	cmd := exec.Command("git", "branch", "--show-current")
	cmd.Dir = dir
	out, err := cmd.Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

// DetectActiveClaudeSessions returns workspace directories that have an active
// Claude Code session, detected by recent .jsonl file modification in
// ~/.claude/projects/{encoded_dir}/. This is a fallback for sessions that
// started before the SessionStart hook was installed.
func DetectActiveClaudeSessions(workspaceDirs []string, maxAge time.Duration) map[string]bool {
	homeDir, err := os.UserHomeDir()
	if err != nil {
		return nil
	}
	projectsDir := filepath.Join(homeDir, ".claude", "projects")
	active := make(map[string]bool)
	cutoff := time.Now().Add(-maxAge)

	for _, dir := range workspaceDirs {
		encoded := encodeClaudeProjectDir(dir)
		projDir := filepath.Join(projectsDir, encoded)
		entries, err := os.ReadDir(projDir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".jsonl") {
				continue
			}
			info, err := e.Info()
			if err != nil {
				continue
			}
			if info.ModTime().After(cutoff) {
				active[dir] = true
				break
			}
		}
	}
	return active
}

// encodeClaudeProjectDir converts a workspace directory path to the format
// used by Claude Code in ~/.claude/projects/.
// e.g., "D:\.workspace\apex_pipeline_branch_01" → "D---workspace-apex-pipeline-branch-01"
func encodeClaudeProjectDir(dir string) string {
	r := strings.NewReplacer(
		":", "-",
		"\\", "-",
		"/", "-",
		".", "-",
		"_", "-",
	)
	return r.Replace(dir)
}

// hasClaudeProcess checks if any claude.exe process is running on the system.
func hasClaudeProcess() bool {
	cmd := exec.Command("tasklist.exe", "/FI", "IMAGENAME eq claude.exe", "/FO", "CSV", "/NH")
	out, err := cmd.Output()
	if err != nil {
		return false
	}
	return strings.Contains(string(out), "claude.exe")
}

func gitStatus(dir string) string {
	cmd := exec.Command("git", "status", "--porcelain")
	cmd.Dir = dir
	out, err := cmd.Output()
	if err != nil {
		return "UNKNOWN"
	}
	if len(strings.TrimSpace(string(out))) == 0 {
		return "CLEAN"
	}
	return "DIRTY"
}
