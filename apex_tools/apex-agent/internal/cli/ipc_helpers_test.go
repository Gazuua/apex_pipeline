// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
)

func TestIsDialError(t *testing.T) {
	tests := []struct {
		err  error
		want bool
	}{
		{fmt.Errorf("dial: connection refused"), true},
		{fmt.Errorf("dial: %w", fmt.Errorf("pipe not found")), true},
		{fmt.Errorf("read: unexpected EOF"), false},
		{fmt.Errorf("timeout"), false},
		{nil, false},
	}

	for _, tt := range tests {
		name := "nil"
		if tt.err != nil {
			name = tt.err.Error()
		}
		t.Run(name, func(t *testing.T) {
			if got := isDialError(tt.err); got != tt.want {
				t.Errorf("isDialError(%v) = %v, want %v", tt.err, got, tt.want)
			}
		})
	}
}

// --- parseResponseMap tests (BACKLOG-223) ---

func TestParseResponseMap_Success(t *testing.T) {
	resp := &ipc.Response{
		OK:   true,
		Data: json.RawMessage(`{"found":true,"branch":"feature/test"}`),
	}
	result, err := parseResponseMap(resp)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if found, _ := result["found"].(bool); !found {
		t.Error("expected found=true")
	}
	if branch, _ := result["branch"].(string); branch != "feature/test" {
		t.Errorf("expected branch %q, got %q", "feature/test", branch)
	}
}

func TestParseResponseMap_ErrorField(t *testing.T) {
	resp := &ipc.Response{
		OK:    false,
		Error: "branch not registered",
	}
	_, err := parseResponseMap(resp)
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if err.Error() != "branch not registered" {
		t.Errorf("expected error message %q, got %q", "branch not registered", err.Error())
	}
}

func TestParseResponseMap_InvalidJSON(t *testing.T) {
	resp := &ipc.Response{
		OK:   true,
		Data: json.RawMessage(`not-json`),
	}
	_, err := parseResponseMap(resp)
	if err == nil {
		t.Fatal("expected error for invalid JSON, got nil")
	}
}

func TestParseResponseMap_EmptyData(t *testing.T) {
	resp := &ipc.Response{
		OK:   true,
		Data: json.RawMessage(`{}`),
	}
	result, err := parseResponseMap(resp)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(result) != 0 {
		t.Errorf("expected empty map, got %v", result)
	}
}

func TestParseResponseMap_NullData(t *testing.T) {
	resp := &ipc.Response{
		OK:   true,
		Data: json.RawMessage(`null`),
	}
	// json.Unmarshal(null, &map) sets map to nil — no error
	result, err := parseResponseMap(resp)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result != nil {
		t.Errorf("expected nil map, got %v", result)
	}
}

func TestParseResponseMap_NestedData(t *testing.T) {
	resp := &ipc.Response{
		OK:   true,
		Data: json.RawMessage(`{"status":"ok","count":42,"nested":{"key":"val"}}`),
	}
	result, err := parseResponseMap(resp)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if status, _ := result["status"].(string); status != "ok" {
		t.Errorf("expected status %q, got %q", "ok", status)
	}
	// JSON numbers decode as float64
	if count, _ := result["count"].(float64); count != 42 {
		t.Errorf("expected count 42, got %v", count)
	}
}
