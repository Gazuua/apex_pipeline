// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
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

// TestHook_ValidateMergeRequiresLock verifies that hook.validate-merge blocks
// gh pr merge when no file-based merge lock is held, and allows it when the lock exists.
func TestHook_ValidateMergeRequiresLock(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Create a temp queue directory for lock isolation.
	queueDir := filepath.Join(env.Dir, "queue")
	if err := os.MkdirAll(queueDir, 0o755); err != nil {
		t.Fatalf("create queue dir: %v", err)
	}

	// Point hook to our isolated queue directory.
	t.Setenv("APEX_BUILD_QUEUE_DIR", queueDir)

	sendValidateMerge := func(cmd, cwd string) *ipc.Response {
		t.Helper()
		resp, err := env.Client.Send(ctx, "hook", "validate-merge",
			map[string]string{"command": cmd, "cwd": cwd}, "")
		if err != nil {
			t.Fatalf("validate-merge(%q): transport error: %v", cmd, err)
		}
		return resp
	}

	// Without lock → blocked.
	resp := sendValidateMerge("gh pr merge --squash --admin", env.Dir)
	if resp.OK {
		t.Error("gh pr merge without lock: expected blocked (NOT OK), got OK")
	}

	// Create the merge.lock directory to simulate lock acquisition.
	lockDir := filepath.Join(queueDir, "merge.lock")
	if err := os.MkdirAll(lockDir, 0o755); err != nil {
		t.Fatalf("create merge.lock: %v", err)
	}

	// With lock → allowed.
	resp = sendValidateMerge("gh pr merge --squash --admin", env.Dir)
	if !resp.OK {
		t.Errorf("gh pr merge with lock: expected allowed (OK), got error: %s", resp.Error)
	}

	// Remove the lock.
	if err := os.RemoveAll(lockDir); err != nil {
		t.Fatalf("remove merge.lock: %v", err)
	}

	// After lock release → blocked again.
	resp = sendValidateMerge("gh pr merge --squash --admin", env.Dir)
	if resp.OK {
		t.Error("gh pr merge after lock release: expected blocked (NOT OK), got OK")
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
