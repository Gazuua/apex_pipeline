// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workspace

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
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
