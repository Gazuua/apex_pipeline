// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

// TestHandoff_FullLifecycle verifies the complete state machine:
// unregistered → started → design-notified → implementing → merge-notified.
func TestHandoff_FullLifecycle(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	branch := "feature/test-1"
	workspace := "ws1"

	// Step 1: notify-start
	resp, err := env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"summary":   "test handoff lifecycle",
	}, "")
	if err != nil {
		t.Fatalf("notify-start: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-start: expected OK, got error: %s", resp.Error)
	}
	var startData map[string]any
	if err := json.Unmarshal(resp.Data, &startData); err != nil {
		t.Fatalf("notify-start: unmarshal: %v", err)
	}
	if _, ok := startData["notification_id"]; !ok {
		t.Error("notify-start: response missing 'notification_id'")
	}

	// Step 2: get-status → "started"
	assertStatus(t, env, ctx, branch, "started")

	// Step 3: notify-transition type="design"
	resp, err = env.Client.Send(ctx, "handoff", "notify-transition", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"type":      "design",
		"summary":   "design decided",
	}, "")
	if err != nil {
		t.Fatalf("notify-transition design: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-transition design: expected OK, got error: %s", resp.Error)
	}

	// Step 4: get-status → "design-notified"
	assertStatus(t, env, ctx, branch, "design-notified")

	// Step 5: notify-transition type="plan"
	resp, err = env.Client.Send(ctx, "handoff", "notify-transition", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"type":      "plan",
		"summary":   "plan ready",
	}, "")
	if err != nil {
		t.Fatalf("notify-transition plan: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-transition plan: expected OK, got error: %s", resp.Error)
	}

	// Step 6: get-status → "implementing"
	assertStatus(t, env, ctx, branch, "implementing")

	// Step 7: notify-transition type="merge"
	resp, err = env.Client.Send(ctx, "handoff", "notify-transition", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"type":      "merge",
		"summary":   "ready to merge",
	}, "")
	if err != nil {
		t.Fatalf("notify-transition merge: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-transition merge: expected OK, got error: %s", resp.Error)
	}

	// Step 8: get-status → "merge-notified"
	assertStatus(t, env, ctx, branch, "merge-notified")
}

// TestHandoff_GateEnforcement verifies gate rules for commit and edit validation.
func TestHandoff_GateEnforcement(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	branch := "feature/gate-test"
	workspace := "ws-gate"

	// Step 1: validate-commit on unregistered branch → should be blocked
	resp, err := env.Client.Send(ctx, "handoff", "validate-commit", map[string]any{
		"branch": branch,
	}, "")
	if err != nil {
		t.Fatalf("validate-commit unregistered: transport error: %v", err)
	}
	if resp.OK {
		t.Error("validate-commit on unregistered branch: expected blocked (NOT OK), got OK")
	}

	// Step 2: notify-start (skip_design=false → status becomes "started")
	resp, err = env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":      branch,
		"workspace":   workspace,
		"summary":     "gate enforcement test",
		"skip_design": false,
	}, "")
	if err != nil {
		t.Fatalf("notify-start: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-start: expected OK, got error: %s", resp.Error)
	}

	// Step 3: validate-commit → allowed (branch is now registered)
	resp, err = env.Client.Send(ctx, "handoff", "validate-commit", map[string]any{
		"branch": branch,
	}, "")
	if err != nil {
		t.Fatalf("validate-commit registered: transport error: %v", err)
	}
	if !resp.OK {
		t.Errorf("validate-commit registered: expected allowed (OK), got error: %s", resp.Error)
	}

	// Step 4: validate-edit on .go file → blocked (status="started", not "implementing")
	resp, err = env.Client.Send(ctx, "handoff", "validate-edit", map[string]any{
		"branch":    branch,
		"file_path": "internal/foo/bar.go",
	}, "")
	if err != nil {
		t.Fatalf("validate-edit .go started: transport error: %v", err)
	}
	if resp.OK {
		t.Error("validate-edit .go in started status: expected blocked (NOT OK), got OK")
	}

	// Step 5: validate-edit on .md file → allowed (non-source file)
	resp, err = env.Client.Send(ctx, "handoff", "validate-edit", map[string]any{
		"branch":    branch,
		"file_path": "docs/README.md",
	}, "")
	if err != nil {
		t.Fatalf("validate-edit .md: transport error: %v", err)
	}
	if !resp.OK {
		t.Errorf("validate-edit .md: expected allowed (OK), got error: %s", resp.Error)
	}

	// Step 6: transition to implementing (design → plan)
	resp, err = env.Client.Send(ctx, "handoff", "notify-transition", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"type":      "design",
		"summary":   "design done",
	}, "")
	if err != nil {
		t.Fatalf("notify-transition design: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-transition design: expected OK, got error: %s", resp.Error)
	}

	resp, err = env.Client.Send(ctx, "handoff", "notify-transition", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"type":      "plan",
		"summary":   "plan ready",
	}, "")
	if err != nil {
		t.Fatalf("notify-transition plan: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-transition plan: expected OK, got error: %s", resp.Error)
	}

	assertStatus(t, env, ctx, branch, "implementing")

	// Step 7: validate-edit on .go file → allowed (now implementing)
	resp, err = env.Client.Send(ctx, "handoff", "validate-edit", map[string]any{
		"branch":    branch,
		"file_path": "internal/foo/bar.go",
	}, "")
	if err != nil {
		t.Fatalf("validate-edit .go implementing: transport error: %v", err)
	}
	if !resp.OK {
		t.Errorf("validate-edit .go implementing: expected allowed (OK), got error: %s", resp.Error)
	}
}

// TestHandoff_MultiWorkspace verifies cross-workspace notification and ack flow.
//
// Scenario: branch-A starts, branch-B starts; branch-A transitions to design.
// branch-B sees branch-A's notifications. Branch-A wants to merge — it must ack
// branch-B's start notification first. Then validate-merge-gate is allowed.
func TestHandoff_MultiWorkspace(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	branchA := "feature/branch-a"
	branchB := "feature/branch-b"

	// Step 1: notify-start for branch-A in workspace ws1
	resp, err := env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":    branchA,
		"workspace": "ws1",
		"summary":   "branch A work",
	}, "")
	if err != nil {
		t.Fatalf("notify-start branch-A: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-start branch-A: expected OK, got error: %s", resp.Error)
	}

	// Step 2: notify-start for branch-B in workspace ws2
	resp, err = env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":    branchB,
		"workspace": "ws2",
		"summary":   "branch B work",
	}, "")
	if err != nil {
		t.Fatalf("notify-start branch-B: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-start branch-B: expected OK, got error: %s", resp.Error)
	}

	// Step 3: transition branch-A to design (creates a notification visible to branch-B)
	resp, err = env.Client.Send(ctx, "handoff", "notify-transition", map[string]any{
		"branch":    branchA,
		"workspace": "ws1",
		"type":      "design",
		"summary":   "branch A design done",
	}, "")
	if err != nil {
		t.Fatalf("notify-transition design branch-A: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-transition design branch-A: expected OK, got error: %s", resp.Error)
	}

	// Step 4: check for branch-B → should see notifications from branch-A
	resp, err = env.Client.Send(ctx, "handoff", "check", map[string]any{
		"branch": branchB,
	}, "")
	if err != nil {
		t.Fatalf("check branch-B: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("check branch-B: expected OK, got error: %s", resp.Error)
	}
	var checkData map[string]any
	if err := json.Unmarshal(resp.Data, &checkData); err != nil {
		t.Fatalf("check branch-B: unmarshal: %v", err)
	}
	notifsRaw, ok := checkData["notifications"]
	if !ok {
		t.Fatal("check branch-B: response missing 'notifications'")
	}
	notifList, ok := notifsRaw.([]any)
	if !ok {
		t.Fatalf("check branch-B: 'notifications' is not array, got %T", notifsRaw)
	}
	// branch-B should see notifications from branch-A (start + design)
	if len(notifList) == 0 {
		t.Error("check branch-B: expected notifications from branch-A, got none")
	}

	// Step 5: branch-B acks all notifications from branch-A
	for _, n := range notifList {
		nm, ok := n.(map[string]any)
		if !ok {
			continue
		}
		var nid int
		if idRaw, ok2 := nm["ID"]; ok2 {
			nid = int(idRaw.(float64))
		} else if idRaw, ok2 := nm["id"]; ok2 {
			nid = int(idRaw.(float64))
		} else {
			t.Fatal("notification missing ID field")
		}

		ackResp, ackErr := env.Client.Send(ctx, "handoff", "ack", map[string]any{
			"notification_id": nid,
			"branch":          branchB,
			"action":          "no-impact",
		}, "")
		if ackErr != nil {
			t.Fatalf("ack notif %d: transport error: %v", nid, ackErr)
		}
		if !ackResp.OK {
			t.Fatalf("ack notif %d: expected OK, got error: %s", nid, ackResp.Error)
		}
		var ackData map[string]any
		if err := json.Unmarshal(ackResp.Data, &ackData); err != nil {
			t.Fatalf("ack notif %d: unmarshal: %v", nid, err)
		}
		if ackData["ok"] != true {
			t.Errorf("ack notif %d: expected ok=true, got %v", nid, ackData["ok"])
		}
	}

	// validate-merge-gate for branch-A:
	// branch-A needs to ack branch-B's start notification before merging.
	// First, check what branch-A has unacked.
	resp, err = env.Client.Send(ctx, "handoff", "check", map[string]any{
		"branch": branchA,
	}, "")
	if err != nil {
		t.Fatalf("check branch-A: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("check branch-A: expected OK, got error: %s", resp.Error)
	}
	var checkDataA map[string]any
	if err := json.Unmarshal(resp.Data, &checkDataA); err != nil {
		t.Fatalf("check branch-A: unmarshal: %v", err)
	}
	notifsARaw := checkDataA["notifications"].([]any)

	// Step 6: branch-A acks all pending notifications (branch-B's start)
	for _, n := range notifsARaw {
		nm := n.(map[string]any)
		var nid int
		if idRaw, ok := nm["ID"]; ok {
			nid = int(idRaw.(float64))
		} else if idRaw, ok := nm["id"]; ok {
			nid = int(idRaw.(float64))
		}
		env.Client.Send(ctx, "handoff", "ack", map[string]any{ //nolint:errcheck
			"notification_id": nid,
			"branch":          branchA,
			"action":          "no-impact",
		}, "")
	}

	// validate-merge-gate for branch-A → allowed (all notifications acked)
	resp, err = env.Client.Send(ctx, "handoff", "validate-merge-gate", map[string]any{
		"branch": branchA,
	}, "")
	if err != nil {
		t.Fatalf("validate-merge-gate branch-A: transport error: %v", err)
	}
	if !resp.OK {
		t.Errorf("validate-merge-gate branch-A: expected allowed (OK), got error: %s", resp.Error)
	}
}

// TestHandoff_ResolveBranch verifies workspace ID primary + git_branch fallback lookup.
func TestHandoff_ResolveBranch(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Register with git_branch
	resp, err := env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":     "ws_01",
		"workspace":  "ws_01",
		"git_branch": "feature/test-resolve",
		"summary":    "resolve test",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("notify-start: %v / %s", err, resp.Error)
	}

	// 1차: workspace ID로 조회 → found
	resp, err = env.Client.Send(ctx, "handoff", "resolve-branch", map[string]any{
		"workspace_id": "ws_01",
		"git_branch":   "",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("resolve by workspace: %v / %s", err, resp.Error)
	}
	var result map[string]any
	json.Unmarshal(resp.Data, &result)
	if result["found"] != true {
		t.Error("workspace ID lookup should find the branch")
	}
	if result["branch"] != "ws_01" {
		t.Errorf("expected branch 'ws_01', got %v", result["branch"])
	}

	// 2차: 존재하지 않는 workspace → git_branch fallback
	resp, err = env.Client.Send(ctx, "handoff", "resolve-branch", map[string]any{
		"workspace_id": "nonexistent",
		"git_branch":   "feature/test-resolve",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("resolve by git_branch: %v / %s", err, resp.Error)
	}
	json.Unmarshal(resp.Data, &result)
	if result["found"] != true {
		t.Error("git_branch fallback should find the branch")
	}
	if result["branch"] != "ws_01" {
		t.Errorf("fallback should return 'ws_01', got %v", result["branch"])
	}

	// 3차: 둘 다 없음 → not found
	resp, err = env.Client.Send(ctx, "handoff", "resolve-branch", map[string]any{
		"workspace_id": "ghost",
		"git_branch":   "feature/nonexistent",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("resolve not found: %v / %s", err, resp.Error)
	}
	json.Unmarshal(resp.Data, &result)
	if result["found"] != false {
		t.Error("nonexistent branch should not be found")
	}
}

// assertStatus is a helper that sends handoff.get-status and asserts the returned status.
func assertStatus(t *testing.T, env *testenv.TestEnv, ctx context.Context, branch, want string) {
	t.Helper()
	resp, err := env.Client.Send(ctx, "handoff", "get-status", map[string]any{
		"branch": branch,
	}, "")
	if err != nil {
		t.Fatalf("get-status(%s): transport error: %v", branch, err)
	}
	if !resp.OK {
		t.Fatalf("get-status(%s): expected OK, got error: %s", branch, resp.Error)
	}
	var data map[string]string
	if err := json.Unmarshal(resp.Data, &data); err != nil {
		t.Fatalf("get-status(%s): unmarshal: %v", branch, err)
	}
	if data["status"] != want {
		t.Errorf("get-status(%s): expected %q, got %q", branch, want, data["status"])
	}
}
