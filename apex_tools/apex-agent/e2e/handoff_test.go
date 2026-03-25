// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"strings"
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
	if startData["status"] != "started" {
		t.Errorf("notify-start: expected status='started', got %v", startData["status"])
	}

	// Step 2: get-status → "STARTED"
	assertStatus(t, env, ctx, branch, "STARTED")

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

	// Step 4: get-status → "DESIGN_NOTIFIED"
	assertStatus(t, env, ctx, branch, "DESIGN_NOTIFIED")

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

	// Step 6: get-status → "IMPLEMENTING"
	assertStatus(t, env, ctx, branch, "IMPLEMENTING")

	// Step 7: notify-merge
	resp, err = env.Client.Send(ctx, "handoff", "notify-merge", map[string]any{
		"branch":    branch,
		"workspace": workspace,
		"summary":   "ready to merge",
	}, "")
	if err != nil {
		t.Fatalf("notify-merge: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("notify-merge: expected OK, got error: %s", resp.Error)
	}

	// Step 8: get-status → "" (removed from active_branches)
	assertStatus(t, env, ctx, branch, "")
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

	// Step 2: notify-start (skip_design=false → status becomes "STARTED")
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

	// Step 4: validate-edit on .go file → blocked (status="STARTED", not "IMPLEMENTING")
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

	assertStatus(t, env, ctx, branch, "IMPLEMENTING")

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

// TestHandoff_MultiWorkspace verifies cross-workspace state management and merge gate.
//
// Scenario: branch-A starts, branch-B starts; branch-A transitions to design.
// Two branches registered. Branch-A transitions to design then merge gate passes.
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

	// Step 3: transition branch-A to design
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

	// Step 4: validate-merge-gate for branch-A → allowed (no FIXING backlogs)
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

	// Register with branch_name
	resp, err := env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":      "ws_01",
		"workspace":   "ws_01",
		"branch_name": "feature/test-resolve",
		"summary":     "resolve test",
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
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
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
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
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
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if result["found"] != false {
		t.Error("nonexistent branch should not be found")
	}
}

// TestHandoff_BacklogFixingAndMergeGate verifies:
//  1. notify-start with backlog_ids → junction INSERT + FIXING status transition
//  2. merge gate blocks when FIXING backlogs exist
//  3. resolve removes FIXING → merge gate passes
func TestHandoff_BacklogFixingAndMergeGate(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	branch := "feature/fixing-test"

	// Step 1: Add a backlog item
	resp, err := env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id": 1, "title": "Gate Test Bug", "severity": "MAJOR", "timeframe": "NOW",
		"scope": "CORE", "type": "BUG", "description": "test FIXING gate",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("backlog.add: %v / %s", err, resp.Error)
	}

	// Step 2: notify-start with backlog_ids → should set backlog to FIXING
	resp, err = env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":      branch,
		"workspace":   "ws1",
		"summary":     "fixing gate test",
		"backlog_ids": []int{1},
		"skip_design": true,
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("notify-start: %v / %s", err, resp.Error)
	}

	// Step 3: Verify backlog is FIXING
	resp, err = env.Client.Send(ctx, "backlog", "check", map[string]any{"id": 1}, "")
	if err != nil || !resp.OK {
		t.Fatalf("backlog.check: %v / %s", err, resp.Error)
	}
	var checkData map[string]any
	if err := json.Unmarshal(resp.Data, &checkData); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if checkData["status"] != "FIXING" {
		t.Errorf("expected backlog status FIXING, got %v", checkData["status"])
	}

	// Step 4: Verify branch has backlog in junction
	resp, err = env.Client.Send(ctx, "handoff", "get-branch", map[string]any{"branch": branch}, "")
	if err != nil || !resp.OK {
		t.Fatalf("get-branch: %v / %s", err, resp.Error)
	}
	var branchData map[string]any
	if err := json.Unmarshal(resp.Data, &branchData); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	raw, _ := json.Marshal(branchData["branch"])
	var branchInfo map[string]any
	if err := json.Unmarshal(raw, &branchInfo); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	backlogIDs, _ := branchInfo["BacklogIDs"].([]any)
	if len(backlogIDs) != 1 {
		t.Errorf("expected 1 backlog ID in junction, got %d", len(backlogIDs))
	}

	// Step 5: Merge gate should BLOCK (FIXING backlog exists)
	resp, err = env.Client.Send(ctx, "handoff", "validate-merge-gate", map[string]any{
		"branch": branch,
	}, "")
	if err != nil {
		t.Fatalf("validate-merge-gate: %v", err)
	}
	if resp.OK {
		t.Error("merge gate should block when FIXING backlogs exist")
	}
	if !strings.Contains(resp.Error, "미해결 백로그") {
		t.Errorf("expected FIXING block message, got: %s", resp.Error)
	}

	// Step 6: Resolve the backlog → merge gate should pass
	resp, err = env.Client.Send(ctx, "backlog", "resolve", map[string]any{
		"id": 1, "resolution": "FIXED",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("backlog.resolve: %v / %s", err, resp.Error)
	}

	resp, err = env.Client.Send(ctx, "handoff", "validate-merge-gate", map[string]any{
		"branch": branch,
	}, "")
	if err != nil {
		t.Fatalf("validate-merge-gate after resolve: %v", err)
	}
	if !resp.OK {
		t.Errorf("merge gate should pass after resolve: %s", resp.Error)
	}
}

// TestHandoff_StartJob verifies non-backlog job registration.
func TestHandoff_StartJob(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	branch := "feature/job-test"

	// Register without backlog
	resp, err := env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch":      branch,
		"workspace":    "ws1",
		"branch_name":  "feature/job-test",
		"summary":      "job mode test",
		"backlog_ids":  []int{},
		"skip_design":  true,
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("notify-start job: %v / %s", err, resp.Error)
	}

	// Verify status = implementing (skip-design)
	assertStatus(t, env, ctx, branch, "IMPLEMENTING")

	// Verify no backlog junction
	resp, err = env.Client.Send(ctx, "handoff", "get-branch", map[string]any{"branch": branch}, "")
	if err != nil || !resp.OK {
		t.Fatalf("get-branch: %v / %s", err, resp.Error)
	}
	var data map[string]any
	if err := json.Unmarshal(resp.Data, &data); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	raw, _ := json.Marshal(data["branch"])
	var info map[string]any
	if err := json.Unmarshal(raw, &info); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	backlogIDs, _ := info["BacklogIDs"].([]any)
	if len(backlogIDs) != 0 {
		t.Errorf("job mode should have 0 backlog IDs, got %d", len(backlogIDs))
	}

	// Verify git_branch stored
	if info["GitBranch"] != "feature/job-test" {
		t.Errorf("expected git_branch 'feature/job-test', got %v", info["GitBranch"])
	}

	// Merge gate should pass (no FIXING backlogs)
	resp, err = env.Client.Send(ctx, "handoff", "validate-merge-gate", map[string]any{
		"branch": branch,
	}, "")
	if err != nil {
		t.Fatalf("validate-merge-gate: %v", err)
	}
	if !resp.OK {
		t.Errorf("merge gate should pass for job with no backlogs: %s", resp.Error)
	}
}

// TestHandoff_BacklogCheckJunction verifies backlog-check uses junction table.
func TestHandoff_BacklogCheckJunction(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Add backlog item
	env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id": 50, "title": "Junction Check", "severity": "MINOR", "timeframe": "NOW",
		"scope": "TOOLS", "type": "TEST", "description": "junction test",
	}, "")

	// Before registration: available
	resp, err := env.Client.Send(ctx, "handoff", "backlog-check", map[string]any{"backlog_id": 50}, "")
	if err != nil || !resp.OK {
		t.Fatalf("backlog-check before: %v / %s", err, resp.Error)
	}
	var result map[string]any
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if result["available"] != true {
		t.Error("backlog 50 should be available before registration")
	}

	// Register branch with backlog 50
	env.Client.Send(ctx, "handoff", "notify-start", map[string]any{
		"branch": "feature/junction-50", "workspace": "ws1",
		"summary": "junction test", "backlog_ids": []int{50}, "skip_design": true,
	}, "")

	// After registration: in use
	resp, err = env.Client.Send(ctx, "handoff", "backlog-check", map[string]any{"backlog_id": 50}, "")
	if err != nil || !resp.OK {
		t.Fatalf("backlog-check after: %v / %s", err, resp.Error)
	}
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if result["available"] != false {
		t.Error("backlog 50 should be in use after registration")
	}
	if result["branch"] != "feature/junction-50" {
		t.Errorf("expected branch 'feature/junction-50', got %v", result["branch"])
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
