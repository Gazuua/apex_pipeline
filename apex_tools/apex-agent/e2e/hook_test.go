// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"os"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
)

// TestHook_ValidateBuildBlocksAndAllows verifies that hook.validate-build
// correctly blocks direct build tool invocations and allows safe commands.
func TestHook_ValidateBuildBlocksAndAllows(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	sendValidateBuild := func(cmd string) *ipc.Response {
		t.Helper()
		resp, err := env.Client.Send(ctx, "hook", "validate-build",
			map[string]string{"command": cmd}, "")
		if err != nil {
			t.Fatalf("validate-build(%q): transport error: %v", cmd, err)
		}
		return resp
	}

	// cmake should be blocked.
	resp := sendValidateBuild("cmake --build out/build")
	if resp.OK {
		t.Error("cmake --build: expected blocked (NOT OK), got OK")
	}

	// ninja should be blocked.
	resp = sendValidateBuild("ninja -C out")
	if resp.OK {
		t.Error("ninja: expected blocked (NOT OK), got OK")
	}

	// Read-only cat command should be allowed.
	resp = sendValidateBuild("cat foo")
	if !resp.OK {
		t.Errorf("cat foo: expected allowed (OK), got error: %s", resp.Error)
	}
	var data map[string]string
	if err := json.Unmarshal(resp.Data, &data); err == nil {
		if data["status"] != "allowed" {
			t.Errorf("cat foo: expected status=allowed, got %q", data["status"])
		}
	}

	// queue-lock.sh should be allowed.
	resp = sendValidateBuild("/path/to/queue-lock.sh build debug")
	if !resp.OK {
		t.Errorf("queue-lock.sh: expected allowed (OK), got error: %s", resp.Error)
	}
}

// TestHook_ValidateMergeRequiresLock verifies that merge lock is enforced
// via DB-based queue (queue.status). The CLI hook_cmd.go checks queue status
// to determine if merge is allowed.
func TestHook_ValidateMergeRequiresLock(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	checkMergeStatus := func() bool {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "status",
			map[string]string{"channel": "merge"}, "")
		if err != nil {
			t.Fatalf("queue.status: transport error: %v", err)
		}
		if !resp.OK {
			t.Fatalf("queue.status: error: %s", resp.Error)
		}
		var result map[string]any
		if err := json.Unmarshal(resp.Data, &result); err != nil {
			t.Fatalf("queue.status: unmarshal: %v", err)
		}
		return result["active"] != nil
	}

	// Without lock → no active entry.
	if checkMergeStatus() {
		t.Error("merge channel should have no active entry initially")
	}

	// Acquire merge lock.
	resp, err := env.Client.Send(ctx, "queue", "try-acquire",
		map[string]any{"channel": "merge", "branch": "test-branch", "pid": os.Getpid()}, "")
	if err != nil {
		t.Fatalf("queue.acquire: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("queue.acquire: error: %s", resp.Error)
	}

	// With lock → active entry exists.
	if !checkMergeStatus() {
		t.Error("merge channel should have active entry after acquire")
	}

	// Release merge lock.
	resp, err = env.Client.Send(ctx, "queue", "release",
		map[string]any{"channel": "merge"}, "")
	if err != nil {
		t.Fatalf("queue.release: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("queue.release: error: %s", resp.Error)
	}

	// After release → no active entry.
	if checkMergeStatus() {
		t.Error("merge channel should have no active entry after release")
	}
}

// TestHook_MalformedInput verifies that hook.validate-build handles edge-case
// inputs gracefully without crashing the daemon.
func TestHook_MalformedInput(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Empty params map (command field defaults to empty string) → allowed (no crash).
	resp, err := env.Client.Send(ctx, "hook", "validate-build",
		map[string]string{}, "")
	if err != nil {
		t.Fatalf("empty params: transport error: %v", err)
	}
	// Empty command is explicitly allowed by ValidateBuild.
	if !resp.OK {
		t.Errorf("empty command: expected OK (empty command is allowed), got error: %s", resp.Error)
	}

	// nil params → the server will receive null JSON; unmarshal should fail gracefully.
	resp, err = env.Client.Send(ctx, "hook", "validate-build", nil, "")
	if err != nil {
		t.Fatalf("nil params: transport error: %v", err)
	}
	// null JSON cannot unmarshal into struct → error response expected, no crash.
	if resp == nil {
		t.Fatal("nil params: got nil response")
	}

	// Invalid JSON shape: send a JSON array instead of an object.
	// The daemon should return an error response rather than crashing.
	resp, err = env.Client.Send(ctx, "hook", "validate-build",
		json.RawMessage(`[1,2,3]`), "")
	if err != nil {
		t.Fatalf("invalid JSON shape: transport error: %v", err)
	}
	if resp == nil {
		t.Fatal("invalid JSON shape: got nil response")
	}

	// Key invariant: daemon is still alive and reachable after malformed inputs.
	verifyResp, verifyErr := env.Client.Send(ctx, "daemon", "version", nil, "")
	if verifyErr != nil {
		t.Fatalf("daemon unreachable after malformed input: %v", verifyErr)
	}
	if !verifyResp.OK {
		t.Errorf("daemon unhealthy after malformed input: %s", verifyResp.Error)
	}
}
