// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package plugin

import (
	"encoding/json"
	"os"
	"path/filepath"
	"time"
)

const (
	pluginID      = "apex-auto-review@apex-local"
	marketplaceID = "apex-local"
)

// Setup ensures the apex-auto-review plugin is registered in the user's Claude
// settings. Idempotent: skips if installPath and version already match.
func Setup(workspaceRoot string) error {
	homeDir, err := os.UserHomeDir()
	if err != nil {
		return err
	}

	claudeDir := filepath.Join(homeDir, ".claude")
	pluginsDir := filepath.Join(claudeDir, "plugins")
	if err := os.MkdirAll(pluginsDir, 0o755); err != nil {
		return err
	}

	pluginPath := filepath.Join(workspaceRoot, "apex_tools", "claude-plugin")
	marketplacePath := filepath.Join(workspaceRoot, "apex_tools")

	// Read plugin version from plugin.json
	version := readPluginVersion(pluginPath)

	// Quick check: if already installed with same path+version, skip
	installedFile := filepath.Join(pluginsDir, "installed_plugins.json")
	if isAlreadyInstalled(installedFile, pluginPath, version) {
		return nil
	}

	// 1. known_marketplaces.json
	knownFile := filepath.Join(pluginsDir, "known_marketplaces.json")
	if err := updateKnownMarketplaces(knownFile, marketplacePath); err != nil {
		return err
	}

	// 2. installed_plugins.json
	if err := updateInstalledPlugins(installedFile, pluginPath, version); err != nil {
		return err
	}

	// 3. settings.json (enabledPlugins)
	settingsFile := filepath.Join(claudeDir, "settings.json")
	return updateEnabledPlugins(settingsFile)
}

// readPluginVersion reads the version field from the plugin's plugin.json.
// Returns "1.0.0" if the file cannot be read or version is missing.
func readPluginVersion(pluginPath string) string {
	data, err := os.ReadFile(filepath.Join(pluginPath, ".claude-plugin", "plugin.json"))
	if err != nil {
		return "1.0.0"
	}
	var pj struct {
		Version string `json:"version"`
	}
	if err := json.Unmarshal(data, &pj); err != nil || pj.Version == "" {
		return "1.0.0"
	}
	return pj.Version
}

// isAlreadyInstalled returns true if the plugin entry in installedFile already
// has the exact same installPath and version (no update needed).
func isAlreadyInstalled(installedFile, pluginPath, version string) bool {
	data, err := os.ReadFile(installedFile)
	if err != nil {
		return false
	}
	var installed installedPluginsFile
	if err := json.Unmarshal(data, &installed); err != nil {
		return false
	}
	entries, ok := installed.Plugins[pluginID]
	if !ok || len(entries) == 0 {
		return false
	}
	e := entries[0]
	return filepath.Clean(e.InstallPath) == filepath.Clean(pluginPath) && e.Version == version
}

// updateKnownMarketplaces ensures the marketplace entry exists and points to
// the correct marketplacePath.
func updateKnownMarketplaces(knownFile, marketplacePath string) error {
	known := readJSONMap(knownFile)

	entry, ok := known[marketplaceID].(map[string]interface{})
	if ok {
		// Check if path already matches
		if src, ok := entry["source"].(map[string]interface{}); ok {
			if existing, _ := src["path"].(string); filepath.Clean(existing) == filepath.Clean(marketplacePath) {
				// No update needed for known_marketplaces
				if inst, _ := entry["installLocation"].(string); filepath.Clean(inst) == filepath.Clean(marketplacePath) {
					return nil
				}
			}
		}
	}

	known[marketplaceID] = map[string]interface{}{
		"source": map[string]interface{}{
			"source": "directory",
			"path":   marketplacePath,
		},
		"installLocation": marketplacePath,
		"lastUpdated":     time.Now().UTC().Format(time.RFC3339),
	}
	return writeJSONFile(knownFile, known)
}

// updateInstalledPlugins updates the installed_plugins.json with the new path
// and version, creating or updating the entry as needed.
func updateInstalledPlugins(installedFile, pluginPath, version string) error {
	var installed installedPluginsFile
	if data, err := os.ReadFile(installedFile); err == nil {
		_ = json.Unmarshal(data, &installed)
	}
	if installed.Version == 0 {
		installed.Version = 2
	}
	if installed.Plugins == nil {
		installed.Plugins = make(map[string][]pluginEntry)
	}

	now := time.Now().UTC().Format(time.RFC3339)
	entries := installed.Plugins[pluginID]
	if len(entries) == 0 {
		installed.Plugins[pluginID] = []pluginEntry{{
			Scope:       "project",
			InstallPath: pluginPath,
			Version:     version,
			InstalledAt: now,
			LastUpdated: now,
		}}
	} else {
		e := &entries[0]
		e.InstallPath = pluginPath
		e.Version = version
		e.LastUpdated = now
		installed.Plugins[pluginID] = entries
	}

	return writeJSONFile(installedFile, installed)
}

// updateEnabledPlugins adds the plugin to the enabledPlugins map in
// ~/.claude/settings.json if it is not already present.
func updateEnabledPlugins(settingsFile string) error {
	settings := readJSONMap(settingsFile)
	enabledPlugins, ok := settings["enabledPlugins"].(map[string]interface{})
	if !ok {
		enabledPlugins = make(map[string]interface{})
		settings["enabledPlugins"] = enabledPlugins
	}
	if _, exists := enabledPlugins[pluginID]; exists {
		return nil
	}
	enabledPlugins[pluginID] = true
	return writeJSONFile(settingsFile, settings)
}

// --- JSON helpers ---

type installedPluginsFile struct {
	Version int                      `json:"version"`
	Plugins map[string][]pluginEntry `json:"plugins"`
}

type pluginEntry struct {
	Scope       string `json:"scope"`
	InstallPath string `json:"installPath"`
	Version     string `json:"version"`
	InstalledAt string `json:"installedAt"`
	LastUpdated string `json:"lastUpdated"`
}

// readJSONMap reads a JSON file into a generic map. Returns an empty map on
// any error so callers can always proceed to write.
func readJSONMap(path string) map[string]interface{} {
	data, err := os.ReadFile(path)
	if err != nil {
		return make(map[string]interface{})
	}
	var m map[string]interface{}
	if err := json.Unmarshal(data, &m); err != nil {
		return make(map[string]interface{})
	}
	return m
}

// writeJSONFile marshals v as pretty JSON (2-space indent) and writes to path.
func writeJSONFile(path string, v interface{}) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	return os.WriteFile(path, data, 0o644)
}
