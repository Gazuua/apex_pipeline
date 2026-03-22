// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
)

func TestServer_HandleRequest(t *testing.T) {
	addr := testAddr() + "-server"
	if runtime.GOOS != "windows" {
		addr = "/tmp/apex-agent-test-server.sock"
	}

	router := daemon.NewRouter()
	router.RegisterModule("echo", func(reg daemon.RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"reply": "pong"}, nil
		})
	})

	srv := NewServer(addr, router)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go srv.Serve(ctx)
	time.Sleep(50 * time.Millisecond)

	conn, err := Dial(addr)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}
	defer conn.Close()

	req := &Request{Module: "echo", Action: "ping", Workspace: "test"}
	WriteMessage(conn, req)

	var resp Response
	ReadMessage(conn, &resp)

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
