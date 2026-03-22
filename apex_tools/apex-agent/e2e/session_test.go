// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
	apexctx "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/context"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/plugin"
)

// TestContext_OutputFormat verifies that context.Generate produces the expected
// section headers and includes git branch info.
//
// #15
func TestContext_OutputFormat(t *testing.T) {
	env := testenv.New(t)
	repoDir := env.InitGitRepo(t)

	// Isolate handoff dir so the function doesn't read the host machine's state.
	handoffDir := filepath.Join(env.Dir, "handoff")
	t.Setenv("APEX_HANDOFF_DIR", handoffDir)

	output := apexctx.Generate(repoDir)

	requiredSections := []string{
		"=== Project Context (auto-injected) ===",
		"--- Git Status ---",
		"Branch:",
		"--- Handoff Storage ---",
		"=== End Project Context ===",
	}
	for _, want := range requiredSections {
		if !strings.Contains(output, want) {
			t.Errorf("context output missing expected string %q\nFull output:\n%s", want, output)
		}
	}
}

// TestPlugin_IdempotentSetup verifies that plugin.Setup is idempotent:
// calling it twice leaves installed_plugins.json unchanged on the second call.
//
// #16
func TestPlugin_IdempotentSetup(t *testing.T) {
	// Create an isolated fake home + workspace.
	fakeHome := t.TempDir()
	fakeWorkspace := t.TempDir()

	// Redirect all home-resolution env vars for cross-platform isolation.
	t.Setenv("HOME", fakeHome)
	t.Setenv("USERPROFILE", fakeHome)
	t.Setenv("LOCALAPPDATA", filepath.Join(fakeHome, "AppData", "Local"))

	// Create minimal plugin.json that plugin.Setup reads to get the version.
	pluginSrcDir := filepath.Join(fakeWorkspace, "apex_tools", "claude-plugin", ".claude-plugin")
	if err := os.MkdirAll(pluginSrcDir, 0o755); err != nil {
		t.Fatalf("mkdir plugin src dir: %v", err)
	}
	pluginJSON := `{"version":"1.0.0","name":"apex-auto-review"}`
	if err := os.WriteFile(filepath.Join(pluginSrcDir, "plugin.json"), []byte(pluginJSON), 0o644); err != nil {
		t.Fatalf("write plugin.json: %v", err)
	}

	// First call — should create installed_plugins.json.
	if err := plugin.Setup(fakeWorkspace); err != nil {
		t.Fatalf("plugin.Setup first call: %v", err)
	}

	installedFile := filepath.Join(fakeHome, ".claude", "plugins", "installed_plugins.json")
	firstContent, err := os.ReadFile(installedFile)
	if err != nil {
		t.Fatalf("read installed_plugins.json after first Setup: %v", err)
	}

	// Verify the file is valid JSON and contains our plugin entry.
	var parsed map[string]any
	if err := json.Unmarshal(firstContent, &parsed); err != nil {
		t.Fatalf("installed_plugins.json is not valid JSON: %v", err)
	}
	if _, ok := parsed["plugins"]; !ok {
		t.Error("installed_plugins.json missing 'plugins' key")
	}

	// Second call — should be a no-op (idempotent).
	if err := plugin.Setup(fakeWorkspace); err != nil {
		t.Fatalf("plugin.Setup second call: %v", err)
	}

	secondContent, err := os.ReadFile(installedFile)
	if err != nil {
		t.Fatalf("read installed_plugins.json after second Setup: %v", err)
	}

	// installed_plugins.json must not have changed (same bytes).
	if string(firstContent) != string(secondContent) {
		t.Errorf("plugin.Setup is not idempotent: installed_plugins.json changed on second call\nBefore:\n%s\nAfter:\n%s",
			firstContent, secondContent)
	}
}
