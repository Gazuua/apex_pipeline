// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func testSocketAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-daemon-test`
	}
	return "/tmp/apex-agent-daemon-test.sock"
}

func TestDaemon_StartStop(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr(),
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	time.Sleep(100 * time.Millisecond)

	// PID file should exist.
	if _, err := os.Stat(cfg.PIDFilePath); os.IsNotExist(err) {
		t.Error("PID file not created")
	}

	cancel()
	if err := <-done; err != nil {
		t.Errorf("Run returned error: %v", err)
	}

	// PID file should be cleaned up.
	if _, err := os.Stat(cfg.PIDFilePath); !os.IsNotExist(err) {
		t.Error("PID file not cleaned up")
	}
}

func TestDaemon_RegisterModule(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr() + "-mod",
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	started := false
	stopped := false

	d.Register(&mockModule{
		name:    "test",
		onStart: func() { started = true },
		onStop:  func() { stopped = true },
	})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()
	time.Sleep(500 * time.Millisecond)

	if !started {
		t.Error("module OnStart not called")
	}

	cancel()
	<-done

	if !stopped {
		t.Error("module OnStop not called")
	}
}

// mockModule for testing.
type mockModule struct {
	name    string
	onStart func()
	onStop  func()
}

func (m *mockModule) Name() string                      { return m.name }
func (m *mockModule) RegisterRoutes(reg RouteRegistrar)  {}
func (m *mockModule) RegisterSchema(mig *store.Migrator) {}
func (m *mockModule) OnStart(ctx context.Context) error {
	if m.onStart != nil {
		m.onStart()
	}
	return nil
}
func (m *mockModule) OnStop() error {
	if m.onStop != nil {
		m.onStop()
	}
	return nil
}
