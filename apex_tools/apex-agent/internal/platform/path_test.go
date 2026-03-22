// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package platform

import (
	"path/filepath"
	"runtime"
	"testing"
)

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
