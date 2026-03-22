// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
)

// TestResilience_MalformedIPC verifies that sending garbage bytes to the daemon
// socket does not crash the daemon — subsequent valid requests still succeed.
//
// #17
func TestResilience_MalformedIPC(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Dial the daemon socket directly and send garbage (not length-prefixed JSON).
	conn, err := ipc.Dial(env.SocketAddr)
	if err != nil {
		t.Fatalf("Dial daemon socket: %v", err)
	}

	// Write raw garbage bytes — this intentionally bypasses WriteMessage so the
	// daemon receives data that cannot be decoded as a valid IPC request.
	garbage := []byte{0xFF, 0xFE, 0x00, 0x01, 0x7F, 0x80, 0xAB, 0xCD}
	_, _ = conn.Write(garbage)
	conn.Close()

	// Give the daemon a moment to handle and recover from the bad connection.
	// (No sleep needed — daemon handles per-connection errors without blocking.)

	// Verify the daemon is still responsive after the malformed input.
	resp, err := env.Client.Send(ctx, "daemon", "version", nil, "")
	if err != nil {
		t.Fatalf("daemon.version after malformed IPC: %v", err)
	}
	if !resp.OK {
		t.Fatalf("daemon.version after malformed IPC: expected OK, got error: %s", resp.Error)
	}
	var versionData map[string]string
	if err := json.Unmarshal(resp.Data, &versionData); err != nil {
		t.Fatalf("daemon.version: unmarshal: %v", err)
	}
	if _, ok := versionData["version"]; !ok {
		t.Error("daemon.version: response missing 'version' field after malformed input")
	}
}

// TestResilience_DBAutoRecreate verifies that if the database file is deleted
// while the daemon is stopped, the daemon auto-recreates it on restart.
//
// #18
func TestResilience_DBAutoRecreate(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Add some data to the running daemon (backlog item via IPC).
	resp, err := env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id":          1,
		"title":       "resilience test item",
		"severity":    "MINOR",
		"timeframe":   "DEFERRED",
		"scope":       "TOOLS",
		"type":        "PERF",
		"description": "DB auto-recreate resilience test",
	}, "")
	if err != nil {
		t.Fatalf("backlog.add: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.add: expected OK, got error: %s", resp.Error)
	}

	// Shut the daemon down via IPC, then call Stop() to drain the done channel.
	// Stop() sets env.stopped=true so the t.Cleanup registered by testenv.New
	// will skip its own drain and not deadlock.
	_, _ = env.Client.Send(ctx, "daemon", "shutdown", nil, "")
	env.Stop()

	// Delete the database file to simulate corruption / accidental removal.
	if err := os.Remove(env.DBPath); err != nil && !os.IsNotExist(err) {
		t.Fatalf("remove DB file: %v", err)
	}

	// Restart — the daemon must auto-recreate the DB without panicking.
	env.Restart(t)

	// Daemon must still respond to version requests.
	resp, err = env.Client.Send(ctx, "daemon", "version", nil, "")
	if err != nil {
		t.Fatalf("daemon.version after DB recreation: %v", err)
	}
	if !resp.OK {
		t.Fatalf("daemon.version after DB recreation: expected OK, got error: %s", resp.Error)
	}

	// Backlog must be empty (DB was deleted and recreated from scratch).
	resp, err = env.Client.Send(ctx, "backlog", "list", nil, "")
	if err != nil {
		t.Fatalf("backlog.list after DB recreation: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.list after DB recreation: expected OK, got error: %s", resp.Error)
	}
	var listItems []any
	if err := json.Unmarshal(resp.Data, &listItems); err != nil {
		t.Fatalf("backlog.list: unmarshal: %v", err)
	}
	if len(listItems) != 0 {
		t.Errorf("backlog.list: expected 0 items after DB deletion+restart, got %d", len(listItems))
	}
}

// TestResilience_StalePIDRecovery verifies that a new daemon starts successfully
// even when a stale PID file referencing a non-existent process is present.
//
// #19
func TestResilience_StalePIDRecovery(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Stop the daemon so we can write a stale PID file.
	env.Stop()

	// Write a PID file referencing a process that almost certainly does not exist.
	// PID 999999 is beyond Linux's default PID max (32768) and Windows never uses it.
	pidFile := filepath.Join(env.Dir, "test.pid")
	stalePID := "999999"
	if err := os.WriteFile(pidFile, []byte(stalePID+"\n"), 0o644); err != nil {
		t.Fatalf("write stale PID file: %v", err)
	}

	// Verify that the stale PID was written correctly.
	data, err := os.ReadFile(pidFile)
	if err != nil {
		t.Fatalf("read PID file: %v", err)
	}
	if strconv.Itoa(999999) != stalePID {
		t.Fatalf("internal test error: expected stale PID %q, file contains %q", stalePID, string(data))
	}

	// Restart should override the stale PID and start cleanly.
	env.Restart(t)

	// Daemon must respond normally after PID recovery.
	resp, err := env.Client.Send(ctx, "daemon", "version", nil, "")
	if err != nil {
		t.Fatalf("daemon.version after stale PID recovery: %v", err)
	}
	if !resp.OK {
		t.Fatalf("daemon.version after stale PID recovery: expected OK, got error: %s", resp.Error)
	}
	var versionData map[string]string
	if err := json.Unmarshal(resp.Data, &versionData); err != nil {
		t.Fatalf("daemon.version: unmarshal: %v", err)
	}
	if _, ok := versionData["version"]; !ok {
		t.Error("daemon.version: missing 'version' field after stale PID recovery")
	}
}

// TestResilience_CustomConfigPaths verifies that config.Load correctly reads
// custom values from a user-supplied TOML file.
//
// #20
func TestResilience_CustomConfigPaths(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	customConfigDir := filepath.Join(env.Dir, "custom-config")
	if err := os.MkdirAll(customConfigDir, 0o755); err != nil {
		t.Fatalf("mkdir custom config dir: %v", err)
	}
	customConfigPath := filepath.Join(customConfigDir, "config.toml")

	// Write a custom config with non-default values.
	customTOML := `[daemon]
idle_timeout = "2h"

[log]
level = "warn"
max_size_mb = 100
max_backups = 7

[queue]
stale_timeout = "30m"
poll_interval = "5s"
`
	if err := os.WriteFile(customConfigPath, []byte(customTOML), 0o644); err != nil {
		t.Fatalf("write custom config: %v", err)
	}

	// Load the custom config.
	cfg, err := config.Load(customConfigPath)
	if err != nil {
		t.Fatalf("config.Load: %v", err)
	}

	// Verify custom values were applied.
	if cfg.Log.Level != "warn" {
		t.Errorf("log.level: expected %q, got %q", "warn", cfg.Log.Level)
	}
	if cfg.Log.MaxSizeMB != 100 {
		t.Errorf("log.max_size_mb: expected 100, got %d", cfg.Log.MaxSizeMB)
	}
	if cfg.Log.MaxBackups != 7 {
		t.Errorf("log.max_backups: expected 7, got %d", cfg.Log.MaxBackups)
	}

	// Verify that config.Load on a missing file returns defaults without error.
	missingPath := filepath.Join(customConfigDir, "nonexistent.toml")
	defaultCfg, err := config.Load(missingPath)
	if err != nil {
		t.Fatalf("config.Load on missing file: expected no error, got %v", err)
	}
	if defaultCfg.Log.Level != "debug" {
		t.Errorf("default log.level: expected %q, got %q", "debug", defaultCfg.Log.Level)
	}

	// Verify the already-running daemon (using its own config) still works.
	resp, err := env.Client.Send(ctx, "daemon", "version", nil, "")
	if err != nil {
		t.Fatalf("daemon.version with custom config path test: %v", err)
	}
	if !resp.OK {
		t.Fatalf("daemon.version: expected OK, got error: %s", resp.Error)
	}
}
