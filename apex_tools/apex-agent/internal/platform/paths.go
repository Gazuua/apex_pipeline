// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package platform

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// AppName is the application identifier used for directory and pipe names.
const (
	AppName    = "apex-agent"
	pipeName   = `\\.\pipe\apex-agent`
	socketName = "apex-agent.sock"
	dbName     = "apex-agent.db"
	pidName    = "apex-agent.pid"
)

// DataDir returns the platform-specific data directory for apex-agent.
// Windows: %LOCALAPPDATA%/apex-agent, Unix: $XDG_DATA_HOME/apex-agent or ~/.local/share/apex-agent.
func DataDir() string {
	if runtime.GOOS == "windows" {
		return filepath.Join(os.Getenv("LOCALAPPDATA"), AppName)
	}
	if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
		return filepath.Join(xdg, AppName)
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", AppName)
}

// DBPath returns the full path to the SQLite database file.
func DBPath() string { return filepath.Join(DataDir(), dbName) }

// PIDFilePath returns the full path to the daemon PID file.
func PIDFilePath() string { return filepath.Join(DataDir(), pidName) }

// SocketPath returns the platform-specific IPC socket address.
// Windows: Named Pipe, Unix: Unix domain socket in XDG_RUNTIME_DIR (preferred) or /tmp.
func SocketPath() string {
	if runtime.GOOS == "windows" {
		return pipeName
	}
	if xdg := os.Getenv("XDG_RUNTIME_DIR"); xdg != "" {
		return filepath.Join(xdg, socketName)
	}
	return filepath.Join(os.TempDir(), socketName)
}

// EnsureDataDir creates the data directory if it doesn't exist.
func EnsureDataDir() error {
	return os.MkdirAll(DataDir(), 0o755)
}

// WorkspaceID extracts the workspace branch identifier from a project root path.
// e.g., "/path/to/apex_pipeline_branch_02" → "branch_02"
func WorkspaceID(root string) string {
	base := filepath.Base(root)
	if strings.HasPrefix(base, "apex_pipeline_") {
		return strings.TrimPrefix(base, "apex_pipeline_")
	}
	return base
}

// GitCurrentBranch returns the current git branch name for the given root directory.
// Returns empty string and error if git fails or root is empty.
func GitCurrentBranch(root string) (string, error) {
	args := []string{"branch", "--show-current"}
	if root != "" {
		args = []string{"-C", root, "branch", "--show-current"}
	}
	out, err := exec.Command("git", args...).Output()
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(out)), nil
}

// NormalizePath normalizes a file path for the current platform.
// On Windows, handles MSYS-style paths (/c/Users/...) and forward slashes.
func NormalizePath(p string) string {
	if runtime.GOOS == "windows" && len(p) >= 3 && p[0] == '/' && p[2] == '/' {
		drive := strings.ToUpper(string(p[1]))
		p = drive + ":" + p[2:]
	}
	return filepath.Clean(filepath.FromSlash(p))
}
