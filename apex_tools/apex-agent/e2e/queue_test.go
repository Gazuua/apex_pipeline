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

// TestQueue_BuildBenchmarkMutualExclusion verifies that build and benchmark
// share the "build" channel and are mutually exclusive — holding one blocks the other.
//
// Scenario:
//  1. "build" acquires "build" channel → succeeds
//  2. "benchmark" tries to acquire "build" channel → fails (held by build)
//  3. "build" releases
//  4. "benchmark" acquires → succeeds
//  5. "build" tries to acquire → fails (held by benchmark)
//  6. "benchmark" releases
//  7. Both can now acquire freely
func TestQueue_BuildBenchmarkMutualExclusion(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	const channel = "build"
	const buildBranch = "feature/build-work"
	const benchBranch = "feature/bench-work"

	tryAcquire := func(branch string, pid int) bool {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
			"channel": channel,
			"branch":  branch,
			"pid":     pid,
		}, "")
		if err != nil {
			t.Fatalf("try-acquire %s: %v", branch, err)
		}
		if !resp.OK {
			t.Fatalf("try-acquire %s: %s", branch, resp.Error)
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
			t.Fatalf("release: %v", err)
		}
		if !resp.OK {
			t.Fatalf("release: %s", resp.Error)
		}
	}

	// Step 1: "build" acquires.
	if !tryAcquire(buildBranch, 88001) {
		t.Fatal("build should acquire free channel")
	}

	// Step 2: "benchmark" can't acquire — same channel held by build.
	if tryAcquire(benchBranch, 88002) {
		t.Fatal("benchmark should NOT acquire while build holds the lock")
	}

	// Step 3: release build.
	release()

	// Step 4: benchmark now acquires.
	if !tryAcquire(benchBranch, 88002) {
		t.Fatal("benchmark should acquire after build released")
	}

	// Step 5: build can't acquire — held by benchmark.
	if tryAcquire(buildBranch, 88001) {
		t.Fatal("build should NOT acquire while benchmark holds the lock")
	}

	// Step 6: release benchmark.
	release()

	// Step 7: both channels free — verify.
	if !tryAcquire(buildBranch, 88001) {
		t.Fatal("build should acquire after full release")
	}
	release()
}

// TestQueue_SequentialBenchmarks verifies that multiple benchmark processes
// execute one at a time (not concurrently), enforced by the shared "build" lock.
//
// Scenario: 3 benchmarks queue up. Each gets the lock after the previous releases.
func TestQueue_SequentialBenchmarks(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	const channel = "build"
	benchmarks := []struct {
		branch string
		pid    int
	}{
		{"feature/bench-mpsc", 77001},
		{"feature/bench-spsc", 77002},
		{"feature/bench-ring", 77003},
	}

	tryAcquire := func(branch string, pid int) bool {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
			"channel": channel,
			"branch":  branch,
			"pid":     pid,
		}, "")
		if err != nil {
			t.Fatalf("try-acquire %s: %v", branch, err)
		}
		if !resp.OK {
			t.Fatalf("try-acquire %s: %s", branch, resp.Error)
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
			t.Fatalf("release: %v", err)
		}
		if !resp.OK {
			t.Fatalf("release: %s", resp.Error)
		}
	}

	// First benchmark acquires.
	if !tryAcquire(benchmarks[0].branch, benchmarks[0].pid) {
		t.Fatal("first benchmark should acquire free channel")
	}

	// Remaining benchmarks queue up (acquire fails).
	for i := 1; i < len(benchmarks); i++ {
		if tryAcquire(benchmarks[i].branch, benchmarks[i].pid) {
			t.Fatalf("benchmark %d should NOT acquire while another holds the lock", i)
		}
	}

	// Release and acquire in FIFO order.
	for i := 1; i < len(benchmarks); i++ {
		release()
		if !tryAcquire(benchmarks[i].branch, benchmarks[i].pid) {
			t.Fatalf("benchmark %d should acquire after previous released", i)
		}
	}

	// Final release.
	release()

	// Channel must be free.
	resp, err := env.Client.Send(ctx, "queue", "status", map[string]any{
		"channel": channel,
	}, "")
	if err != nil {
		t.Fatalf("final status: %v", err)
	}
	var status map[string]any
	if err := json.Unmarshal(resp.Data, &status); err != nil {
		t.Fatalf("final status: unmarshal: %v", err)
	}
	if status["active"] != nil {
		t.Errorf("channel should be free, got active=%v", status["active"])
	}
}

// TestQueue_MergeBenchmarkIndependent verifies that "merge" and "build" channels
// are independent — holding a build/benchmark lock does NOT block merge operations.
func TestQueue_MergeBenchmarkIndependent(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	const branch = "feature/independent-test"

	tryAcquire := func(channel string, pid int) bool {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "try-acquire", map[string]any{
			"channel": channel,
			"branch":  branch,
			"pid":     pid,
		}, "")
		if err != nil {
			t.Fatalf("try-acquire %s: %v", channel, err)
		}
		if !resp.OK {
			t.Fatalf("try-acquire %s: %s", channel, resp.Error)
		}
		var data map[string]any
		if err := json.Unmarshal(resp.Data, &data); err != nil {
			t.Fatalf("try-acquire %s: unmarshal: %v", channel, err)
		}
		return data["acquired"] == true
	}

	releaseChannel := func(channel string) {
		t.Helper()
		resp, err := env.Client.Send(ctx, "queue", "release", map[string]any{
			"channel": channel,
		}, "")
		if err != nil {
			t.Fatalf("release %s: %v", channel, err)
		}
		if !resp.OK {
			t.Fatalf("release %s: %s", channel, resp.Error)
		}
	}

	// Acquire build channel (simulating benchmark run).
	if !tryAcquire("build", 66001) {
		t.Fatal("build channel should be acquirable")
	}

	// Acquire merge channel — must succeed despite build being held.
	if !tryAcquire("merge", 66002) {
		t.Fatal("merge channel should be independent of build channel")
	}

	// Both held simultaneously — release both.
	releaseChannel("build")
	releaseChannel("merge")
}
