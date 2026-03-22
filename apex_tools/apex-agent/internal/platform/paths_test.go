// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package platform

import (
	"os"
	"path/filepath"
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

func TestEnsureDataDir_Creates(t *testing.T) {
	// EnsureDataDir should not return an error (directory may already exist).
	if err := EnsureDataDir(); err != nil {
		t.Fatalf("EnsureDataDir() error: %v", err)
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

func TestNormalizePath_Absolute(t *testing.T) {
	var input, want string
	if runtime.GOOS == "windows" {
		input = `C:\Users\test\file.txt`
		want = `C:\Users\test\file.txt`
	} else {
		input = "/home/test/file.txt"
		want = "/home/test/file.txt"
	}
	got := NormalizePath(input)
	if got != want {
		t.Errorf("NormalizePath(%q) = %q, want %q", input, got, want)
	}
}

func TestNormalizePath_ForwardSlashes(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows only — forward slash normalization")
	}
	got := NormalizePath("C:/Users/test/file.txt")
	want := filepath.FromSlash("C:/Users/test/file.txt")
	if got != want {
		t.Errorf("NormalizePath() = %q, want %q", got, want)
	}
}

func TestNormalizePath_MSYSPrefix(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows only — MSYS path handling")
	}
	got := NormalizePath("/c/Users/test")
	if got[1] != ':' {
		t.Errorf("NormalizePath(/c/Users/test) = %q, want drive letter path", got)
	}
}
