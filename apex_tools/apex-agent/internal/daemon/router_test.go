// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
)

func TestRouter_Dispatch(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("echo", func(reg RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"pong": ws}, nil
		})
	})

	result, err := r.Dispatch(context.Background(), "echo", "ping", nil, "branch_02")
	if err != nil {
		t.Fatalf("Dispatch error: %v", err)
	}

	data, err := json.Marshal(result)
	if err != nil {
		t.Fatalf("Marshal result: %v", err)
	}
	var m map[string]string
	if err := json.Unmarshal(data, &m); err != nil {
		t.Fatalf("Unmarshal result: %v", err)
	}
	if m["pong"] != "branch_02" {
		t.Errorf("got %v, want pong=branch_02", m)
	}
}

func TestRouter_Dispatch_UnknownModule(t *testing.T) {
	r := NewRouter()
	_, err := r.Dispatch(context.Background(), "missing", "action", nil, "ws")
	if err == nil {
		t.Error("expected error for unknown module")
	}
}

func TestRouter_Dispatch_UnknownAction(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("echo", func(reg RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return nil, nil
		})
	})

	_, err := r.Dispatch(context.Background(), "echo", "missing", nil, "ws")
	if err == nil {
		t.Error("expected error for unknown action")
	}
}

func TestRouter_Dispatch_HandlerError(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("fail", func(reg RouteRegistrar) {
		reg.Handle("boom", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return nil, errors.New("handler failed")
		})
	})

	_, err := r.Dispatch(context.Background(), "fail", "boom", nil, "ws")
	if err == nil || err.Error() != "handler failed" {
		t.Errorf("expected 'handler failed', got %v", err)
	}
}
