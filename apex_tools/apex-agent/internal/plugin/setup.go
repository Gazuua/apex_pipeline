// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package plugin

import (
	"encoding/json"
	"os"
	"path/filepath"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("plugin")

const pluginID = "apex-auto-review@apex-local"

// Setup ensures the apex-auto-review plugin is enabled in the user's Claude
// global settings. Claude Code handles plugin discovery and installation
// natively via extraKnownMarketplaces in the project's .claude/settings.json;
// this function only adds the plugin to enabledPlugins for first-time setup.
// Idempotent: no file write if the entry already exists.
func Setup(_ string) error {
	homeDir, err := os.UserHomeDir()
	if err != nil {
		return err
	}

	settingsFile := filepath.Join(homeDir, ".claude", "settings.json")
	return updateEnabledPlugins(settingsFile)
}

// updateEnabledPlugins adds the plugin to the enabledPlugins map in
// ~/.claude/settings.json if it is not already present.
func updateEnabledPlugins(settingsFile string) error {
	data, err := os.ReadFile(settingsFile)
	if err != nil {
		// settings.json doesn't exist — nothing to write
		// (Claude Code creates it on first run)
		return nil
	}
	var settings map[string]interface{}
	if err := json.Unmarshal(data, &settings); err != nil {
		return nil
	}
	enabledPlugins, ok := settings["enabledPlugins"].(map[string]interface{})
	if !ok {
		enabledPlugins = make(map[string]interface{})
		settings["enabledPlugins"] = enabledPlugins
	}
	if _, exists := enabledPlugins[pluginID]; exists {
		return nil
	}
	ml.Info("enabling plugin in global settings", "plugin", pluginID)
	enabledPlugins[pluginID] = true

	out, err := json.MarshalIndent(settings, "", "  ")
	if err != nil {
		return err
	}
	out = append(out, '\n')
	return os.WriteFile(settingsFile, out, 0o644)
}
