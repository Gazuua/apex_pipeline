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

func TestClient_Send(t *testing.T) {
	addr := testAddr() + "-client"
	if runtime.GOOS != "windows" {
		addr = "/tmp/apex-agent-test-client.sock"
	}

	router := daemon.NewRouter()
	router.RegisterModule("echo", func(reg daemon.RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"ws": ws}, nil
		})
	})

	srv := NewServer(addr, router)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go srv.Serve(ctx)
	time.Sleep(50 * time.Millisecond)

	client := NewClient(addr)
	resp, err := client.Send(context.Background(), "echo", "ping", nil, "branch_01")
	if err != nil {
		t.Fatalf("Send: %v", err)
	}
	if !resp.OK {
		t.Fatalf("not OK: %s", resp.Error)
	}

	var data map[string]string
	json.Unmarshal(resp.Data, &data)
	if data["ws"] != "branch_01" {
		t.Errorf("got %v, want ws=branch_01", data)
	}
}
