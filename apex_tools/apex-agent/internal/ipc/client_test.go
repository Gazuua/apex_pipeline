// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"runtime"
	"testing"
	"time"
)

func TestClient_Send(t *testing.T) {
	addr := testAddr() + "-client"
	if runtime.GOOS != "windows" {
		addr = "/tmp/apex-agent-test-client.sock"
	}

	router := &mockDispatcher{
		handlers: map[string]func(ctx context.Context, params json.RawMessage, ws string) (any, error){
			"echo.ping": func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
				return map[string]string{"ws": ws}, nil
			},
		},
	}

	srv := NewServer(addr, router)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go srv.Serve(ctx)

	// Poll for server readiness instead of fixed sleep.
	client := NewClient(addr)
	ready := false
	for i := 0; i < 100; i++ {
		conn, err := Dial(addr)
		if err == nil {
			conn.Close()
			ready = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !ready {
		t.Fatal("server did not become ready within timeout")
	}
	resp, err := client.Send(context.Background(), "echo", "ping", nil, "branch_01")
	if err != nil {
		t.Fatalf("Send: %v", err)
	}
	if !resp.OK {
		t.Fatalf("not OK: %s", resp.Error)
	}

	var data map[string]string
	if err := json.Unmarshal(resp.Data, &data); err != nil {
		t.Fatalf("Unmarshal response data: %v", err)
	}
	if data["ws"] != "branch_01" {
		t.Errorf("got %v, want ws=branch_01", data)
	}
}
