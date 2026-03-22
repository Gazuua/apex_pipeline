// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package platform

import (
	"os"
	"runtime"
	"strings"
	"testing"
)

func TestDataDir_NotEmpty(t *testing.T) {
	dir := DataDir()
	if dir == "" {
		t.Fatal("DataDir() returned empty string")
	}
	if !strings.Contains(dir, "apex-agent") {
		t.Errorf("DataDir() = %q, want contains 'apex-agent'", dir)
	}
}

func TestDataDir_Windows_UsesLocalAppData(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows only")
	}
	dir := DataDir()
	localAppData := os.Getenv("LOCALAPPDATA")
	if !strings.HasPrefix(dir, localAppData) {
		t.Errorf("DataDir() = %q, want prefix %q", dir, localAppData)
	}
}

func TestDBPath_EndsWithDB(t *testing.T) {
	p := DBPath()
	if !strings.HasSuffix(p, "apex-agent.db") {
		t.Errorf("DBPath() = %q, want suffix 'apex-agent.db'", p)
	}
}

func TestPIDFilePath_EndsWithPID(t *testing.T) {
	p := PIDFilePath()
	if !strings.HasSuffix(p, "apex-agent.pid") {
		t.Errorf("PIDFilePath() = %q, want suffix 'apex-agent.pid'", p)
	}
}

func TestSocketPath_Platform(t *testing.T) {
	p := SocketPath()
	if runtime.GOOS == "windows" {
		if !strings.HasPrefix(p, `\\.\pipe\`) {
			t.Errorf("SocketPath() = %q, want Named Pipe path", p)
		}
	} else {
		if !strings.HasSuffix(p, "apex-agent.sock") {
			t.Errorf("SocketPath() = %q, want suffix 'apex-agent.sock'", p)
		}
	}
}
