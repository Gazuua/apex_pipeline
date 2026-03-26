// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package plugin

import (
	"encoding/json"
	"os"
	"path/filepath"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("plugin")

const (
	pluginID      = "apex-auto-review@apex-local"
	marketplaceID = "apex-local"

	// supportedVersions: known installed_plugins.json format versions.
	// 0 = initial (no version field), 2 = current format.
	installedPluginsCurrentVersion = 2
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

	// Quick check: if already installed with matching version + valid path, skip.
	// This avoids file writes that trigger Claude Code's "reload plugin" prompt
	// when switching between workspace copies of the same repo.
	installedFile := filepath.Join(pluginsDir, "installed_plugins.json")
	if isAlreadyInstalled(installedFile, pluginPath, version) {
		ml.Debug("plugin already installed, skipping", "plugin", pluginID, "version", version)
		// Still ensure enabledPlugins is set (no file write if already present)
		settingsFile := filepath.Join(claudeDir, "settings.json")
		return updateEnabledPlugins(settingsFile)
	}
	ml.Info("installing plugin", "plugin", pluginID, "version", version, "path", pluginPath)

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
// has the matching version and a valid installPath (no update needed).
// Multi-workspace: different workspaces share the same repo content, so we
// only compare versions and check that the existing path is still reachable.
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
	if e.Version != version {
		return false
	}
	// Version matches — check that the existing path is still valid.
	// Even if it points to a different workspace copy, the content is identical.
	pluginJSON := filepath.Join(filepath.Clean(e.InstallPath), ".claude-plugin", "plugin.json")
	_, err = os.Stat(pluginJSON)
	return err == nil
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
// Validates the format version: 0 (initial) and 2 (current) are known.
// Unknown versions produce a warning but proceed (fail-open).
func updateInstalledPlugins(installedFile, pluginPath, version string) error {
	var installed installedPluginsFile
	if data, err := os.ReadFile(installedFile); err == nil {
		_ = json.Unmarshal(data, &installed)
	}

	// Format version check (fail-open: unknown versions produce warning, not error)
	switch installed.Version {
	case 0:
		// Initial format (no version field) — upgrade to current
		installed.Version = installedPluginsCurrentVersion
	case installedPluginsCurrentVersion:
		// Current format — no action needed
	default:
		ml.Warn("unknown installed_plugins.json format version, proceeding anyway",
			"version", installed.Version, "expected", installedPluginsCurrentVersion)
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
