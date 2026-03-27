// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package plugin

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestUpdateEnabledPlugins_AddsEntry(t *testing.T) {
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
}

func TestUpdateEnabledPlugins_Idempotent(t *testing.T) {
	tmp := t.TempDir()
	settingsFile := filepath.Join(tmp, "settings.json")

	if err := os.WriteFile(settingsFile, []byte(`{"someKey":true}`), 0o644); err != nil {
		t.Fatal(err)
	}

	// First call writes
	if err := updateEnabledPlugins(settingsFile); err != nil {
		t.Fatalf("first call: %v", err)
	}

	// Record file content after first write
	before, _ := os.ReadFile(settingsFile)

	// Second call should be no-op (no write)
	if err := updateEnabledPlugins(settingsFile); err != nil {
		t.Fatalf("second call: %v", err)
	}

	after, _ := os.ReadFile(settingsFile)
	if string(before) != string(after) {
		t.Error("expected idempotent: file content changed on second call")
	}
}

func TestUpdateEnabledPlugins_AlreadyEnabled(t *testing.T) {
	tmp := t.TempDir()
	settingsFile := filepath.Join(tmp, "settings.json")

	initial := map[string]interface{}{
		"enabledPlugins": map[string]interface{}{
			pluginID: true,
		},
	}
	data, _ := json.MarshalIndent(initial, "", "  ")
	if err := os.WriteFile(settingsFile, data, 0o644); err != nil {
		t.Fatal(err)
	}

	before, _ := os.ReadFile(settingsFile)

	if err := updateEnabledPlugins(settingsFile); err != nil {
		t.Fatalf("updateEnabledPlugins: %v", err)
	}

	after, _ := os.ReadFile(settingsFile)
	if string(before) != string(after) {
		t.Error("expected no write when plugin already enabled")
	}
}

func TestUpdateEnabledPlugins_MissingFile(t *testing.T) {
	// Should return nil (not error) when settings.json doesn't exist
	err := updateEnabledPlugins("/nonexistent/settings.json")
	if err != nil {
		t.Errorf("expected nil for missing file, got: %v", err)
	}
}
