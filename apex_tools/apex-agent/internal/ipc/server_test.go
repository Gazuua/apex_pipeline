// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"runtime"
	"testing"
	"time"
)

func TestServer_HandleRequest(t *testing.T) {
	addr := testAddr() + "-server"
	if runtime.GOOS != "windows" {
		addr = "/tmp/apex-agent-test-server.sock"
	}

	router := &mockDispatcher{
		handlers: map[string]func(ctx context.Context, params json.RawMessage, ws string) (any, error){
			"echo.ping": func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
				return map[string]string{"reply": "pong"}, nil
			},
		},
	}

	srv := NewServer(addr, router)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go srv.Serve(ctx)

	// Poll for server readiness instead of fixed sleep.
	ready := false
	for i := 0; i < 100; i++ {
		c, err := Dial(addr)
		if err == nil {
			c.Close()
			ready = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !ready {
		t.Fatal("server did not become ready within timeout")
	}

	conn, err := Dial(addr)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}
	defer conn.Close()

	req := &Request{Module: "echo", Action: "ping", Workspace: "test"}
	if err := WriteMessage(conn, req); err != nil {
		t.Fatalf("WriteMessage: %v", err)
	}

	var resp Response
	if err := ReadMessage(conn, &resp); err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}

	if !resp.OK {
		t.Fatalf("response not OK: %s", resp.Error)
	}

	var data map[string]string
	if err := json.Unmarshal(resp.Data, &data); err != nil {
		t.Fatal(err)
	}
	if data["reply"] != "pong" {
		t.Errorf("got %v, want reply=pong", data)
	}
}
