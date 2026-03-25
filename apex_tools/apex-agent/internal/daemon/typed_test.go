// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
)

type echoParams struct {
	Msg string `json:"msg"`
}

func TestTyped_Success(t *testing.T) {
	handler := Typed(func(_ context.Context, p echoParams, ws string) (any, error) {
		return map[string]string{"echo": p.Msg, "ws": ws}, nil
	})

	params := json.RawMessage(`{"msg":"hello"}`)
	result, err := handler(context.Background(), params, "workspace_01")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	m := result.(map[string]string)
	if m["echo"] != "hello" {
		t.Errorf("got echo=%q, want hello", m["echo"])
	}
	if m["ws"] != "workspace_01" {
		t.Errorf("got ws=%q, want workspace_01", m["ws"])
	}
}

func TestTyped_DecodeError(t *testing.T) {
	handler := Typed(func(_ context.Context, p echoParams, _ string) (any, error) {
		t.Fatal("handler should not be called on decode error")
		return nil, nil
	})

	params := json.RawMessage(`{invalid json}`)
	_, err := handler(context.Background(), params, "ws")
	if err == nil {
		t.Fatal("expected decode error")
	}
	if !strings.Contains(err.Error(), "decode params") {
		t.Errorf("error should contain 'decode params', got: %v", err)
	}
}

func TestTyped_EmptyStruct(t *testing.T) {
	type empty struct{}
	handler := Typed(func(_ context.Context, _ empty, _ string) (any, error) {
		return "ok", nil
	})

	// null params should unmarshal to zero-value struct
	result, err := handler(context.Background(), json.RawMessage(`{}`), "ws")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result != "ok" {
		t.Errorf("got %v, want ok", result)
	}
}

func TestNoParams_Success(t *testing.T) {
	handler := NoParams(func(_ context.Context, ws string) (any, error) {
		return map[string]string{"ws": ws}, nil
	})

	// params are ignored
	result, err := handler(context.Background(), json.RawMessage(`{"anything":"ignored"}`), "branch_02")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	m := result.(map[string]string)
	if m["ws"] != "branch_02" {
		t.Errorf("got ws=%q, want branch_02", m["ws"])
	}
}

func TestNoParams_NilParams(t *testing.T) {
	handler := NoParams(func(_ context.Context, _ string) (any, error) {
		return "ok", nil
	})

	result, err := handler(context.Background(), nil, "ws")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result != "ok" {
		t.Errorf("got %v, want ok", result)
	}
}

func TestTyped_RouterIntegration(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("test", func(reg RouteRegistrar) {
		reg.Handle("echo", Typed(func(_ context.Context, p echoParams, _ string) (any, error) {
			return map[string]string{"echo": p.Msg}, nil
		}))
		reg.Handle("version", NoParams(func(_ context.Context, _ string) (any, error) {
			return map[string]string{"v": "1.0"}, nil
		}))
	})

	// Typed handler via router
	result, err := r.Dispatch(context.Background(), "test", "echo", json.RawMessage(`{"msg":"world"}`), "ws")
	if err != nil {
		t.Fatalf("Dispatch error: %v", err)
	}
	m := result.(map[string]string)
	if m["echo"] != "world" {
		t.Errorf("got echo=%q, want world", m["echo"])
	}

	// NoParams handler via router
	result, err = r.Dispatch(context.Background(), "test", "version", nil, "ws")
	if err != nil {
		t.Fatalf("Dispatch error: %v", err)
	}
	m = result.(map[string]string)
	if m["v"] != "1.0" {
		t.Errorf("got v=%q, want 1.0", m["v"])
	}
}
