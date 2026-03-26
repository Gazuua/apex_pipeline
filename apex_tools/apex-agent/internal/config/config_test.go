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
	if cfg.Log.Level != "info" {
		t.Errorf("Log.Level = %q, want 'info'", cfg.Log.Level)
	}
	if !cfg.Log.Audit {
		t.Error("Log.Audit should default to true")
	}
	if !cfg.HTTP.Enabled {
		t.Error("HTTP.Enabled should default to true")
	}
	if cfg.HTTP.Addr != "localhost:7600" {
		t.Errorf("HTTP.Addr = %q, want 'localhost:7600'", cfg.HTTP.Addr)
	}
}

func TestLoad_FileNotFound_ReturnsDefaults(t *testing.T) {
	cfg, err := Load("/nonexistent/config.toml")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Log.Level != "info" {
		t.Error("missing file should return defaults")
	}
}

func TestLoad_PartialOverride(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	os.WriteFile(path, []byte(`
[log]
level = "warn"
`), 0o644)

	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
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

[http]
enabled = false
addr = "127.0.0.1:9090"
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
	if cfg.HTTP.Enabled {
		t.Error("HTTP.Enabled should be false after override")
	}
	if cfg.HTTP.Addr != "127.0.0.1:9090" {
		t.Errorf("HTTP.Addr = %q, want '127.0.0.1:9090'", cfg.HTTP.Addr)
	}
}

func TestWorkspaceConfigDefaults(t *testing.T) {
	cfg := Defaults()
	if cfg.Workspace.RepoName != "apex_pipeline" {
		t.Errorf("want repo_name=apex_pipeline, got %s", cfg.Workspace.RepoName)
	}
	if !cfg.Workspace.ScanOnStart {
		t.Error("want scan_on_start=true")
	}
	if cfg.Workspace.Root != "" {
		t.Errorf("want empty root, got %s", cfg.Workspace.Root)
	}
}

func TestSessionConfigDefaults(t *testing.T) {
	cfg := Defaults()
	if !cfg.Session.Enabled {
		t.Error("want session.enabled=true")
	}
	if cfg.Session.Addr != "localhost:7601" {
		t.Errorf("want addr=localhost:7601, got %s", cfg.Session.Addr)
	}
	if cfg.Session.WatchdogInterval != 1*time.Second {
		t.Errorf("want watchdog_interval=1s, got %v", cfg.Session.WatchdogInterval)
	}
	if cfg.Session.OutputBufferLines != 500 {
		t.Errorf("want output_buffer_lines=500, got %d", cfg.Session.OutputBufferLines)
	}
}

func TestWorkspaceSessionConfigLoad(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	os.WriteFile(path, []byte(`
[workspace]
root = "/tmp/workspaces"
repo_name = "my_project"
scan_on_start = false

[session]
enabled = false
addr = "localhost:9999"
watchdog_interval = "5s"
output_buffer_lines = 100
`), 0o644)
	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Workspace.Root != "/tmp/workspaces" {
		t.Errorf("want root=/tmp/workspaces, got %s", cfg.Workspace.Root)
	}
	if cfg.Workspace.RepoName != "my_project" {
		t.Errorf("want repo_name=my_project, got %s", cfg.Workspace.RepoName)
	}
	if cfg.Workspace.ScanOnStart {
		t.Error("want scan_on_start=false")
	}
	if cfg.Session.Enabled {
		t.Error("want session.enabled=false")
	}
	if cfg.Session.Addr != "localhost:9999" {
		t.Errorf("want addr=localhost:9999, got %s", cfg.Session.Addr)
	}
	if cfg.Session.WatchdogInterval != 5*time.Second {
		t.Errorf("want 5s, got %v", cfg.Session.WatchdogInterval)
	}
	if cfg.Session.OutputBufferLines != 100 {
		t.Errorf("want 100, got %d", cfg.Session.OutputBufferLines)
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
