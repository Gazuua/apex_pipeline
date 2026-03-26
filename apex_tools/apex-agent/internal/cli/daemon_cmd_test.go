// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// TestReadPID_ParseLogic validates the exact parsing logic used by readPID:
// strconv.Atoi(strings.TrimSpace(string(data))).
// readPID itself is coupled to platform.PIDFilePath(), so we exercise the
// same read-then-parse pattern with temp files.
func TestReadPID_ParseLogic(t *testing.T) {
	tests := []struct {
		name    string
		content string
		want    int
		wantErr bool
	}{
		{name: "valid pid with newline", content: "12345\n", want: 12345},
		{name: "valid pid no newline", content: "99", want: 99},
		{name: "trailing spaces", content: "42  \n", want: 42},
		{name: "leading spaces", content: "  42", want: 42},
		{name: "empty", content: "", wantErr: true},
		{name: "non-numeric", content: "abc", wantErr: true},
		{name: "negative", content: "-1", want: -1},
		{name: "zero", content: "0", want: 0},
		{name: "very large", content: "999999999", want: 999999999},
		{name: "float string", content: "123.456", wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			f := filepath.Join(dir, "test.pid")
			if err := os.WriteFile(f, []byte(tt.content), 0o644); err != nil {
				t.Fatalf("write: %v", err)
			}
			data, err := os.ReadFile(f)
			if err != nil {
				t.Fatalf("read: %v", err)
			}
			// Replicate readPID logic exactly
			got, parseErr := strconv.Atoi(strings.TrimSpace(string(data)))
			if (parseErr != nil) != tt.wantErr {
				t.Errorf("parse(%q) error = %v, wantErr %v", tt.content, parseErr, tt.wantErr)
				return
			}
			if !tt.wantErr && got != tt.want {
				t.Errorf("parse(%q) = %d, want %d", tt.content, got, tt.want)
			}
		})
	}
}

// TestReadPID_MissingFile verifies that reading a non-existent PID file returns an error.
func TestReadPID_MissingFile(t *testing.T) {
	_, err := os.ReadFile(filepath.Join(t.TempDir(), "nonexistent.pid"))
	if err == nil {
		t.Error("expected error for non-existent PID file")
	}
}
