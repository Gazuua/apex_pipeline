// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package config

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestDefaults(t *testing.T) {
	cfg := Defaults()
	if cfg.Daemon.IdleTimeout != 30*time.Minute {
		t.Errorf("IdleTimeout = %v, want 30m", cfg.Daemon.IdleTimeout)
	}
	if cfg.Log.Level != "info" {
		t.Errorf("Log.Level = %q, want 'info'", cfg.Log.Level)
	}
	if !cfg.Log.Audit {
		t.Error("Log.Audit should default to true")
	}
}

func TestLoad_FileNotFound_ReturnsDefaults(t *testing.T) {
	cfg, err := Load("/nonexistent/config.toml")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Daemon.IdleTimeout != 30*time.Minute {
		t.Error("missing file should return defaults")
	}
}

func TestLoad_PartialOverride(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	os.WriteFile(path, []byte(`
[daemon]
idle_timeout = "10m"

[log]
level = "warn"
`), 0o644)

	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Daemon.IdleTimeout != 10*time.Minute {
		t.Errorf("IdleTimeout = %v, want 10m", cfg.Daemon.IdleTimeout)
	}
	if cfg.Log.Level != "warn" {
		t.Errorf("Log.Level = %q, want 'warn'", cfg.Log.Level)
	}
	if !cfg.Log.Audit {
		t.Error("Log.Audit should remain default true")
	}
}

func TestLoad_FullConfig(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	os.WriteFile(path, []byte(`
[daemon]
idle_timeout = "1h"
socket_path = "/tmp/test.sock"

[store]
db_path = "/tmp/test.db"

[queue]
stale_timeout = "2h"
poll_interval = "500ms"

[log]
level = "error"
file = "test.log"
max_size_mb = 100
max_backups = 5
audit = false

[build]
command = "make build"
presets = ["debug", "release", "asan"]
`), 0o644)

	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Store.DBPath != "/tmp/test.db" {
		t.Errorf("DBPath = %q", cfg.Store.DBPath)
	}
	if cfg.Queue.StaleTimeout != 2*time.Hour {
		t.Errorf("StaleTimeout = %v", cfg.Queue.StaleTimeout)
	}
	if len(cfg.Build.Presets) != 3 {
		t.Errorf("Presets = %v", cfg.Build.Presets)
	}
}

func TestWriteDefault_CreatesFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	if err := WriteDefault(path); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	if len(data) == 0 {
		t.Error("WriteDefault should create non-empty file")
	}
}
