// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

// TestDaemon_StartIPCRoundtripIdleShutdown verifies the daemon lifecycle:
// IPC version roundtrip, unknown module error response, and IPC-triggered shutdown.
func TestDaemon_StartIPCRoundtripIdleShutdown(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// --- Step 1: Send daemon.version → verify OK with version field ---
	resp, err := env.Client.Send(ctx, "daemon", "version", nil, "")
	if err != nil {
		t.Fatalf("daemon.version: %v", err)
	}
	if !resp.OK {
		t.Fatalf("daemon.version: expected OK, got error: %s", resp.Error)
	}
	var versionData map[string]string
	if err := json.Unmarshal(resp.Data, &versionData); err != nil {
		t.Fatalf("daemon.version: unmarshal data: %v", err)
	}
	if _, ok := versionData["version"]; !ok {
		t.Error("daemon.version: response missing 'version' field")
	}

	// --- Step 2: Send unknown module → verify error response ---
	resp2, err := env.Client.Send(ctx, "nonexistent-module", "some-action", nil, "")
	if err != nil {
		t.Fatalf("unknown module Send: unexpected transport error: %v", err)
	}
	if resp2.OK {
		t.Error("unknown module: expected NOT OK, got OK")
	}
	if resp2.Error == "" {
		t.Error("unknown module: expected non-empty error message")
	}

	// --- Step 3: Send daemon.shutdown → verify response ---
	// The daemon signals internal shutdown; the test framework cleanup will
	// also cancel the context. Both paths converge cleanly.
	resp3, err := env.Client.Send(ctx, "daemon", "shutdown", nil, "")
	if err != nil {
		t.Fatalf("daemon.shutdown: %v", err)
	}
	if !resp3.OK {
		t.Fatalf("daemon.shutdown: expected OK, got error: %s", resp3.Error)
	}
	var shutdownData map[string]string
	if err := json.Unmarshal(resp3.Data, &shutdownData); err != nil {
		t.Fatalf("daemon.shutdown: unmarshal data: %v", err)
	}
	if shutdownData["status"] != "shutting_down" {
		t.Errorf("daemon.shutdown: expected status=shutting_down, got %q", shutdownData["status"])
	}
	// Daemon cleanup is handled by t.Cleanup registered in testenv.New.
}
