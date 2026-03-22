// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

// TestQueue_AcquireReleaseSerialize verifies sequential lock acquire/release via IPC.
//
//  1. try-acquire "build" channel → acquired=true
//  2. status "build" → active entry exists
//  3. try-acquire same channel (different branch) → acquired=false (already held)
//  4. release "build"
//  5. try-acquire again (original branch) → acquired=true (now available)
//  6. release
func TestQueue_AcquireReleaseSerialize(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	const channel = "build"
	const branchA = "feature/queue-a"
	const branchB = "feature/queue-b"

	// Step 1: try-acquire "build" as branch-A → should succeed.
	resp, err := env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
		"channel": channel,
		"branch":  branchA,
		"pid":     99001,
	}, "")
	if err != nil {
		t.Fatalf("try-acquire branch-A: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("try-acquire branch-A: expected OK, got error: %s", resp.Error)
	}
	var acqData map[string]any
	if err := json.Unmarshal(resp.Data, &acqData); err != nil {
		t.Fatalf("try-acquire branch-A: unmarshal: %v", err)
	}
	if acqData["acquired"] != true {
		t.Fatalf("try-acquire branch-A: expected acquired=true, got %v", acqData["acquired"])
	}
	if acqData["channel"] != channel {
		t.Errorf("try-acquire branch-A: expected channel=%q, got %v", channel, acqData["channel"])
	}

	// Step 2: status "build" → active entry must exist.
	resp, err = env.Client.Send(ctx, "queue", "status", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("status: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("status: expected OK, got error: %s", resp.Error)
	}
	var statusData map[string]any
	if err := json.Unmarshal(resp.Data, &statusData); err != nil {
		t.Fatalf("status: unmarshal: %v", err)
	}
	if statusData["active"] == nil {
		t.Fatal("status: expected active entry, got nil")
	}
	activeEntry, ok := statusData["active"].(map[string]any)
	if !ok {
		t.Fatalf("status: active is not a map, got %T", statusData["active"])
	}
	if activeEntry["Branch"] != branchA && activeEntry["branch"] != branchA {
		t.Errorf("status: expected active branch=%q, got %v/%v", branchA, activeEntry["Branch"], activeEntry["branch"])
	}

	// Step 3: try-acquire same channel as branch-B → should fail (channel busy).
	resp, err = env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
		"channel": channel,
		"branch":  branchB,
		"pid":     99002,
	}, "")
	if err != nil {
		t.Fatalf("try-acquire branch-B: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("try-acquire branch-B: expected OK response (acquired=false), got error: %s", resp.Error)
	}
	var acqData2 map[string]any
	if err := json.Unmarshal(resp.Data, &acqData2); err != nil {
		t.Fatalf("try-acquire branch-B: unmarshal: %v", err)
	}
	if acqData2["acquired"] != false {
		t.Fatalf("try-acquire branch-B: expected acquired=false (channel busy), got %v", acqData2["acquired"])
	}

	// Step 4: release "build" by branch-A.
	resp, err = env.Client.Send(ctx, "queue", "release", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("release: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("release: expected OK, got error: %s", resp.Error)
	}
	var relData map[string]any
	if err := json.Unmarshal(resp.Data, &relData); err != nil {
		t.Fatalf("release: unmarshal: %v", err)
	}
	if relData["released"] != true {
		t.Errorf("release: expected released=true, got %v", relData["released"])
	}

	// Step 5: branch-B now tries to acquire again.
	// branch-B is first in the waiting queue, so it should get the lock.
	resp, err = env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
		"channel": channel,
		"branch":  branchB,
		"pid":     99002,
	}, "")
	if err != nil {
		t.Fatalf("try-acquire branch-B after release: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("try-acquire branch-B after release: expected OK, got error: %s", resp.Error)
	}
	var acqData3 map[string]any
	if err := json.Unmarshal(resp.Data, &acqData3); err != nil {
		t.Fatalf("try-acquire branch-B after release: unmarshal: %v", err)
	}
	if acqData3["acquired"] != true {
		t.Fatalf("try-acquire branch-B after release: expected acquired=true (channel free), got %v", acqData3["acquired"])
	}

	// Step 6: release branch-B's lock.
	resp, err = env.Client.Send(ctx, "queue", "release", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("release branch-B: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("release branch-B: expected OK, got error: %s", resp.Error)
	}

	// Final status check: channel must be free.
	resp, err = env.Client.Send(ctx, "queue", "status", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("final status: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("final status: expected OK, got error: %s", resp.Error)
	}
	var finalStatus map[string]any
	if err := json.Unmarshal(resp.Data, &finalStatus); err != nil {
		t.Fatalf("final status: unmarshal: %v", err)
	}
	if finalStatus["active"] != nil {
		t.Errorf("final status: expected no active entry, got %v", finalStatus["active"])
	}
}

// TestQueue_FIFOOrdering verifies that the queue module enforces FIFO lock ordering
// across multiple clients.
//
// Scenario: 5 clients pre-register in the waiting queue while one holder has
// the lock. After each release, the next in line picks up the lock via try-acquire.
// This validates FIFO ordering and mutual exclusion without requiring concurrent
// IPC connections (Windows named pipe constraint).
func TestQueue_FIFOOrdering(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping FIFO ordering test in short mode")
	}

	env := testenv.New(t)
	ctx := context.Background()

	const channel = "fifo"
	const numWaiters = 4

	// Step 1: First client acquires the lock.
	tryAcquire := func(branch string, pid int) bool {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
			"channel": channel,
			"branch":  branch,
			"pid":     pid,
		}, "")
		if err != nil {
			t.Fatalf("try-acquire %s: transport error: %v", branch, err)
		}
		if !resp.OK {
			t.Fatalf("try-acquire %s: expected OK, got: %s", branch, resp.Error)
		}
		var data map[string]any
		if err := json.Unmarshal(resp.Data, &data); err != nil {
			t.Fatalf("try-acquire %s: unmarshal: %v", branch, err)
		}
		return data["acquired"] == true
	}

	release := func() {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "release", map[string]any{
			"channel": channel,
		}, "")
		if err != nil {
			t.Fatalf("release: transport error: %v", err)
		}
		if !resp.OK {
			t.Fatalf("release: expected OK, got: %s", resp.Error)
		}
	}

	// Holder acquires.
	holder := "feature/holder"
	if !tryAcquire(holder, 97000) {
		t.Fatal("holder: expected to acquire free channel")
	}

	// Register numWaiters into the queue while holder is active.
	branches := make([]string, numWaiters)
	for i := range numWaiters {
		branches[i] = "feature/waiter-" + string(rune('a'+i))
		acquired := tryAcquire(branches[i], 97100+i)
		if acquired {
			t.Fatalf("waiter %s: expected acquired=false (channel busy), got true", branches[i])
		}
	}

	// Verify all waiters are in the queue.
	resp, err := env.Client.Send(ctx, "queue", "status", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("status: %v", err)
	}
	if !resp.OK {
		t.Fatalf("status: %s", resp.Error)
	}
	var statusData map[string]any
	if err := json.Unmarshal(resp.Data, &statusData); err != nil {
		t.Fatalf("status unmarshal: %v", err)
	}
	waiting, _ := statusData["waiting"].([]any)
	if len(waiting) != numWaiters {
		t.Fatalf("expected %d waiters, got %d", numWaiters, len(waiting))
	}

	// Step 2: Release holder and verify each waiter gets the lock in FIFO order.
	release() // holder releases

	for idx, branch := range branches {
		pid := 97100 + idx
		// The waiter at the front of the queue should be able to acquire now.
		if !tryAcquire(branch, pid) {
			t.Errorf("waiter %s (index %d): expected acquired=true on release, got false", branch, idx)
			break
		}

		// While this waiter holds the lock, verify via status that the channel is occupied
		// and the remaining waiters are still queued — without calling try-acquire
		// (which would add a NEW waiting entry and pollute the queue).
		statusResp, statusErr := env.Client.Send(ctx, "queue", "status", map[string]any{
			"channel": channel,
		}, "")
		if statusErr == nil && statusResp.OK {
			var sd map[string]any
			if jsonErr := json.Unmarshal(statusResp.Data, &sd); jsonErr == nil {
				if sd["active"] == nil {
					t.Errorf("waiter %s: expected active entry while holding lock", branch)
				}
				remainingWaiters, _ := sd["waiting"].([]any)
				expectedRemaining := numWaiters - idx - 1
				if len(remainingWaiters) != expectedRemaining {
					t.Errorf("waiter %s: expected %d remaining waiters, got %d",
						branch, expectedRemaining, len(remainingWaiters))
				}
			}
		}

		// Release and let the next waiter proceed.
		release()
	}

	// Final state: channel must be free and no waiters remain.
	resp, err = env.Client.Send(ctx, "queue", "status", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("final status: %v", err)
	}
	if !resp.OK {
		t.Fatalf("final status: %s", resp.Error)
	}
	var finalStatus map[string]any
	if err := json.Unmarshal(resp.Data, &finalStatus); err != nil {
		t.Fatalf("final status unmarshal: %v", err)
	}
	if finalStatus["active"] != nil {
		t.Errorf("channel not free after all workers: active=%v", finalStatus["active"])
	}
	finalWaiting, _ := finalStatus["waiting"].([]any)
	if len(finalWaiting) != 0 {
		t.Errorf("unexpected leftover waiters: %d", len(finalWaiting))
	}
}
