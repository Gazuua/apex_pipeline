// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package plugin

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// writePluginJSON creates a temporary plugin directory with a plugin.json.
func writePluginJSON(t *testing.T, dir, version string) string {
	t.Helper()
	pluginDir := filepath.Join(dir, "claude-plugin", ".claude-plugin")
	if err := os.MkdirAll(pluginDir, 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	data := []byte(`{"name":"apex-auto-review","version":"` + version + `"}`)
	if err := os.WriteFile(filepath.Join(pluginDir, "plugin.json"), data, 0o644); err != nil {
		t.Fatalf("write plugin.json: %v", err)
	}
	return filepath.Join(dir, "claude-plugin")
}

func TestReadPluginVersion(t *testing.T) {
	tmp := t.TempDir()
	pluginPath := writePluginJSON(t, tmp, "2.5.1")
	got := readPluginVersion(pluginPath)
	if got != "2.5.1" {
		t.Errorf("readPluginVersion = %q, want %q", got, "2.5.1")
	}
}

func TestReadPluginVersion_MissingFile(t *testing.T) {
	got := readPluginVersion("/nonexistent/path")
	if got != "1.0.0" {
		t.Errorf("readPluginVersion (missing) = %q, want %q", got, "1.0.0")
	}
}

func TestIsAlreadyInstalled_False_WhenFileAbsent(t *testing.T) {
	if isAlreadyInstalled("/nonexistent/installed.json", "/some/path", "1.0.0") {
		t.Error("expected false when file absent")
	}
}

func TestIsAlreadyInstalled_TrueWhenMatch(t *testing.T) {
	tmp := t.TempDir()
	installedFile := filepath.Join(tmp, "installed_plugins.json")
	installed := installedPluginsFile{
		Version: 2,
		Plugins: map[string][]pluginEntry{
			pluginID: {
				{
					Scope:       "project",
					InstallPath: "/some/path",
					Version:     "3.1.0",
				},
			},
		},
	}
	data, _ := json.MarshalIndent(installed, "", "  ")
	if err := os.WriteFile(installedFile, data, 0o644); err != nil {
		t.Fatal(err)
	}

	if !isAlreadyInstalled(installedFile, "/some/path", "3.1.0") {
		t.Error("expected true when path+version match")
	}
}

func TestIsAlreadyInstalled_FalseWhenVersionDiffers(t *testing.T) {
	tmp := t.TempDir()
	installedFile := filepath.Join(tmp, "installed_plugins.json")
	installed := installedPluginsFile{
		Version: 2,
		Plugins: map[string][]pluginEntry{
			pluginID: {
				{
					Scope:       "project",
					InstallPath: "/some/path",
					Version:     "2.0.0",
				},
			},
		},
	}
	data, _ := json.MarshalIndent(installed, "", "  ")
	if err := os.WriteFile(installedFile, data, 0o644); err != nil {
		t.Fatal(err)
	}

	if isAlreadyInstalled(installedFile, "/some/path", "3.1.0") {
		t.Error("expected false when version differs")
	}
}

func TestSetup_CreatesFiles(t *testing.T) {
	tmp := t.TempDir()

	// Create workspace with plugin.json
	pluginsToolsDir := filepath.Join(tmp, "apex_tools", "claude-plugin", ".claude-plugin")
	if err := os.MkdirAll(pluginsToolsDir, 0o755); err != nil {
		t.Fatal(err)
	}
	data := []byte(`{"name":"apex-auto-review","version":"3.1.0"}`)
	if err := os.WriteFile(filepath.Join(pluginsToolsDir, "plugin.json"), data, 0o644); err != nil {
		t.Fatal(err)
	}

	// Override home dir by pointing Setup to our fake home using a wrapper.
	// We test the helper functions directly since Setup uses os.UserHomeDir().
	pluginPath := filepath.Join(tmp, "apex_tools", "claude-plugin")
	installedFile := filepath.Join(tmp, "installed.json")

	// First install
	if err := updateInstalledPlugins(installedFile, pluginPath, "3.1.0"); err != nil {
		t.Fatalf("updateInstalledPlugins: %v", err)
	}

	// Verify file was created
	if _, err := os.Stat(installedFile); err != nil {
		t.Fatalf("installed.json not created: %v", err)
	}

	// isAlreadyInstalled should now return true
	if !isAlreadyInstalled(installedFile, pluginPath, "3.1.0") {
		t.Error("expected isAlreadyInstalled to return true after install")
	}
}

func TestUpdateKnownMarketplaces(t *testing.T) {
	tmp := t.TempDir()
	knownFile := filepath.Join(tmp, "known_marketplaces.json")
	marketplacePath := filepath.Join(tmp, "apex_tools")

	if err := updateKnownMarketplaces(knownFile, marketplacePath); err != nil {
		t.Fatalf("updateKnownMarketplaces: %v", err)
	}

	data, err := os.ReadFile(knownFile)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]interface{}
	if err := json.Unmarshal(data, &m); err != nil {
		t.Fatal(err)
	}
	if _, ok := m[marketplaceID]; !ok {
		t.Errorf("marketplace ID %q not found in known_marketplaces.json", marketplaceID)
	}
}

func TestUpdateEnabledPlugins(t *testing.T) {
	tmp := t.TempDir()
	settingsFile := filepath.Join(tmp, "settings.json")

	// Create minimal settings.json
	if err := os.WriteFile(settingsFile, []byte(`{"someKey":true}`), 0o644); err != nil {
		t.Fatal(err)
	}

	if err := updateEnabledPlugins(settingsFile); err != nil {
		t.Fatalf("updateEnabledPlugins: %v", err)
	}

	data, err := os.ReadFile(settingsFile)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]interface{}
	if err := json.Unmarshal(data, &m); err != nil {
		t.Fatal(err)
	}
	enabled, ok := m["enabledPlugins"].(map[string]interface{})
	if !ok {
		t.Fatal("enabledPlugins not found or wrong type")
	}
	if enabled[pluginID] != true {
		t.Errorf("expected enabledPlugins[%q] = true", pluginID)
	}

	// Calling again should be idempotent (no error)
	if err := updateEnabledPlugins(settingsFile); err != nil {
		t.Fatalf("second updateEnabledPlugins: %v", err)
	}
}
