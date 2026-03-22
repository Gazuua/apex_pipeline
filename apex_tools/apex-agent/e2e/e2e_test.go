// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build e2e

package e2e

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func testAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-e2e`
	}
	return "/tmp/apex-agent-e2e.sock"
}

// echoModule is a minimal test module.
type echoModule struct{}

func (m *echoModule) Name() string { return "echo" }
func (m *echoModule) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
		return map[string]string{"status": "ok", "workspace": ws}, nil
	})
}
func (m *echoModule) RegisterSchema(mig *store.Migrator) {}
func (m *echoModule) OnStart(ctx context.Context) error   { return nil }
func (m *echoModule) OnStop() error                       { return nil }

func TestE2E_FullRoundtrip(t *testing.T) {
	tmpDir := t.TempDir()
	addr := testAddr()

	cfg := daemon.Config{
		DBPath:      filepath.Join(tmpDir, "e2e.db"),
		PIDFilePath: filepath.Join(tmpDir, "e2e.pid"),
		SocketAddr:  addr,
		IdleTimeout: 5 * time.Minute,
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	d.Register(&echoModule{})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	// Wait for readiness.
	time.Sleep(200 * time.Millisecond)

	// PID file exists.
	if _, err := os.Stat(cfg.PIDFilePath); os.IsNotExist(err) {
		t.Fatal("PID file not found")
	}

	// Send request via client.
	client := ipc.NewClient(addr)
	resp, err := client.Send(ctx, "echo", "ping", nil, "branch_02")
	if err != nil {
		t.Fatalf("client.Send: %v", err)
	}
	if !resp.OK {
		t.Fatalf("response not OK: %s", resp.Error)
	}

	var data map[string]string
	json.Unmarshal(resp.Data, &data)
	if data["status"] != "ok" || data["workspace"] != "branch_02" {
		t.Errorf("unexpected response: %v", data)
	}

	// Unknown module returns error.
	resp2, _ := client.Send(ctx, "nonexistent", "action", nil, "ws")
	if resp2.OK {
		t.Error("expected error for unknown module")
	}

	// Shutdown.
	cancel()
	if err := <-done; err != nil {
		t.Errorf("daemon error: %v", err)
	}

	// PID cleaned up.
	if _, err := os.Stat(cfg.PIDFilePath); !os.IsNotExist(err) {
		t.Error("PID file not cleaned up after shutdown")
	}
}
