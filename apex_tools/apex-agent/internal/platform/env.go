// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package platform

import (
	"os"
	"path/filepath"
	"runtime"
)

const (
	AppName    = "apex-agent"
	pipeName   = `\\.\pipe\apex-agent`
	socketName = "apex-agent.sock"
	dbName     = "apex-agent.db"
	pidName    = "apex-agent.pid"
)

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

func DBPath() string      { return filepath.Join(DataDir(), dbName) }
func PIDFilePath() string { return filepath.Join(DataDir(), pidName) }

func SocketPath() string {
	if runtime.GOOS == "windows" {
		return pipeName
	}
	return filepath.Join(os.TempDir(), socketName)
}

func EnsureDataDir() error {
	return os.MkdirAll(DataDir(), 0o755)
}
