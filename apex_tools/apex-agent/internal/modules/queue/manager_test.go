// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package queue_test

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/queue"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func newTestStore(t *testing.T) *store.Store {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = s.Close() })

	mig := store.NewMigrator(s)
	mod := queue.New(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	return s
}

func newTestManager(t *testing.T) *queue.Manager {
	t.Helper()
	s := newTestStore(t)
	return queue.NewManager(s)
}

// TestTryAcquire_Free: free channel → acquired=true, entry has status='active'.
func TestTryAcquire_Free(t *testing.T) {
	m := newTestManager(t)

	acquired, err := m.TryAcquire(context.Background(), "build", "feature/test", 12345)
	if err != nil {
		t.Fatalf("TryAcquire: %v", err)
	}
	if !acquired {
		t.Fatal("expected acquired=true on free channel")
	}

	active, waiting, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active == nil {
		t.Fatal("expected active entry")
	}
	if active.Status != queue.StatusActive {
		t.Errorf("expected status=%s, got %q", queue.StatusActive, active.Status)
	}
	if active.Branch != "feature/test" {
		t.Errorf("expected branch=feature/test, got %q", active.Branch)
	}
	if len(waiting) != 0 {
		t.Errorf("expected no waiting entries, got %d", len(waiting))
	}
}

// TestTryAcquire_Busy: channel has active entry → acquired=false, new entry is 'waiting'.
func TestTryAcquire_Busy(t *testing.T) {
	m := newTestManager(t)

	// First acquirer takes the lock.
	acquired, err := m.TryAcquire(context.Background(), "build", "feature/first", 11111)
	if err != nil {
		t.Fatalf("TryAcquire first: %v", err)
	}
	if !acquired {
		t.Fatal("expected first acquire to succeed")
	}

	// Second acquirer finds channel busy.
	acquired2, err := m.TryAcquire(context.Background(), "build", "feature/second", 22222)
	if err != nil {
		t.Fatalf("TryAcquire second: %v", err)
	}
	if acquired2 {
		t.Fatal("expected acquired=false on busy channel")
	}

	active, waiting, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active == nil || active.Branch != "feature/first" {
		t.Errorf("expected active=feature/first")
	}
	if len(waiting) != 1 {
		t.Errorf("expected 1 waiting entry, got %d", len(waiting))
	}
	if waiting[0].Branch != "feature/second" {
		t.Errorf("expected waiting branch=feature/second, got %q", waiting[0].Branch)
	}
	if waiting[0].Status != queue.StatusWaiting {
		t.Errorf("expected status=%s, got %q", queue.StatusWaiting, waiting[0].Status)
	}
}

// TestRelease_Basic: Acquire → Release → verify status='done'.
func TestRelease_Basic(t *testing.T) {
	m := newTestManager(t)

	acquired, err := m.TryAcquire(context.Background(), "build", "feature/test", 12345)
	if err != nil || !acquired {
		t.Fatalf("TryAcquire: err=%v acquired=%v", err, acquired)
	}

	if err := m.Release("build"); err != nil {
		t.Fatalf("Release: %v", err)
	}

	active, _, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active != nil {
		t.Errorf("expected no active entry after release, got %+v", active)
	}
}

// TestRelease_FreesChannel: Acquire → Release → TryAcquire again → should succeed.
func TestRelease_FreesChannel(t *testing.T) {
	m := newTestManager(t)

	_, _ = m.TryAcquire(context.Background(), "build", "feature/first", 11111)
	_ = m.Release("build")

	acquired, err := m.TryAcquire(context.Background(), "build", "feature/second", 22222)
	if err != nil {
		t.Fatalf("TryAcquire after release: %v", err)
	}
	if !acquired {
		t.Fatal("expected acquired=true on freed channel")
	}
}

// TestStatus_Free: no active → nil active, empty waiting.
func TestStatus_Free(t *testing.T) {
	m := newTestManager(t)

	active, waiting, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active != nil {
		t.Errorf("expected nil active, got %+v", active)
	}
	if len(waiting) != 0 {
		t.Errorf("expected empty waiting, got %d entries", len(waiting))
	}
}

// TestStatus_Busy: active + waiting → returns both.
func TestStatus_Busy(t *testing.T) {
	m := newTestManager(t)

	_, _ = m.TryAcquire(context.Background(), "merge", "feature/a", 11111)
	_, _ = m.TryAcquire(context.Background(), "merge", "feature/b", 22222)
	_, _ = m.TryAcquire(context.Background(), "merge", "feature/c", 33333)

	active, waiting, err := m.Status("merge")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active == nil {
		t.Fatal("expected active entry")
	}
	if active.Branch != "feature/a" {
		t.Errorf("expected active branch=feature/a, got %q", active.Branch)
	}
	if len(waiting) != 2 {
		t.Errorf("expected 2 waiting entries, got %d", len(waiting))
	}
}

// TestCleanupStale_RemovesDeadPID: entry with dead PID is removed.
func TestCleanupStale_RemovesDeadPID(t *testing.T) {
	m := newTestManager(t)

	// PID 999999 is almost certainly dead.
	_, err := m.TryAcquire(context.Background(), "build", "feature/dead", 999999)
	if err != nil {
		t.Fatalf("TryAcquire: %v", err)
	}

	removed, err := m.CleanupStale()
	if err != nil {
		t.Fatalf("CleanupStale: %v", err)
	}
	if removed == 0 {
		t.Error("expected at least 1 stale entry removed")
	}

	active, _, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active != nil {
		t.Errorf("expected no active entry after stale cleanup, got %+v", active)
	}
}

// TestFIFO_Order: two waiters → first one gets lock when released.
func TestFIFO_Order(t *testing.T) {
	m := newTestManager(t)

	// Holder takes the lock.
	_, _ = m.TryAcquire(context.Background(), "build", "feature/holder", 11111)

	// Two waiters register.
	_, _ = m.TryAcquire(context.Background(), "build", "feature/first-waiter", 22222)
	_, _ = m.TryAcquire(context.Background(), "build", "feature/second-waiter", 33333)

	// Release holder.
	_ = m.Release("build")

	// Advance queue: first waiter should now be able to acquire.
	active, waiting, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status after release: %v", err)
	}
	// After release, channel is free (no active). Verify FIFO by trying to advance.
	_ = active
	if len(waiting) != 2 {
		t.Fatalf("expected 2 waiting entries, got %d", len(waiting))
	}
	if waiting[0].Branch != "feature/first-waiter" {
		t.Errorf("expected first waiter to be feature/first-waiter, got %q", waiting[0].Branch)
	}

	// First waiter tries to acquire — should succeed (channel free, it's first in queue).
	ok, err := m.TryAcquire(context.Background(), "build", "feature/first-waiter", 22222)
	if err != nil {
		t.Fatalf("TryAcquire first waiter: %v", err)
	}
	if !ok {
		t.Fatal("expected first waiter to acquire the free lock")
	}

	// Second waiter tries — should fail (first waiter now holds lock).
	ok2, err := m.TryAcquire(context.Background(), "build", "feature/second-waiter", 33333)
	if err != nil {
		t.Fatalf("TryAcquire second waiter: %v", err)
	}
	if ok2 {
		t.Fatal("expected second waiter to be blocked while first waiter holds lock")
	}
}

// TestAcquire_ContextCancel_CleansUpWaitingEntry: context cancellation removes the waiting entry.
func TestAcquire_ContextCancel_CleansUpWaitingEntry(t *testing.T) {
	s := newTestStore(t)
	m := queue.NewManager(s)

	pid := os.Getpid()

	// Hold the lock so that Acquire blocks.
	acquired, err := m.TryAcquire(context.Background(), "build", "feature/holder", pid)
	if err != nil || !acquired {
		t.Fatalf("TryAcquire holder: err=%v acquired=%v", err, acquired)
	}

	// Start Acquire in a goroutine with a cancellable context.
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- m.Acquire(ctx, "build", "feature/waiter", pid)
	}()

	// Give Acquire time to insert its waiting entry.
	time.Sleep(100 * time.Millisecond)

	// Cancel the context.
	cancel()

	// Wait for Acquire to return.
	err = <-done
	if err == nil {
		t.Fatal("expected error from cancelled Acquire, got nil")
	}

	// Verify that the waiting entry was cleaned up.
	_, waiting, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	for _, w := range waiting {
		if w.Branch == "feature/waiter" {
			t.Error("expected waiting entry to be cleaned up after context cancel")
		}
	}
}

// TestAcquire_Timeout: very short timeout causes Acquire to fail and clean up its waiting entry.
func TestAcquire_Timeout(t *testing.T) {
	s := newTestStore(t)
	m := queue.NewManager(s)

	pid := os.Getpid()

	// Hold the lock so that Acquire blocks.
	acquired, err := m.TryAcquire(context.Background(), "build", "feature/holder", pid)
	if err != nil || !acquired {
		t.Fatalf("TryAcquire holder: err=%v acquired=%v", err, acquired)
	}

	// Very short timeout — Acquire should fail almost immediately.
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()

	err = m.Acquire(ctx, "build", "feature/timeout-waiter", pid)
	if err == nil {
		t.Fatal("expected error from timed-out Acquire, got nil")
	}

	// Verify that the waiting entry was cleaned up.
	_, waiting, err := m.Status("build")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	for _, w := range waiting {
		if w.Branch == "feature/timeout-waiter" {
			t.Error("expected waiting entry to be cleaned up after timeout")
		}
	}
}

// TestAcquire_PromotesAfterRelease: Acquire blocks, then succeeds after Release.
func TestAcquire_PromotesAfterRelease(t *testing.T) {
	s := newTestStore(t)
	m := queue.NewManager(s)

	pid := os.Getpid()

	// Hold the lock.
	acquired, err := m.TryAcquire(context.Background(), "merge", "feature/holder", pid)
	if err != nil || !acquired {
		t.Fatalf("TryAcquire holder: err=%v acquired=%v", err, acquired)
	}

	// Start Acquire in a goroutine.
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- m.Acquire(ctx, "merge", "feature/waiter", pid)
	}()

	// Give Acquire time to register as waiting.
	time.Sleep(100 * time.Millisecond)

	// Release the lock.
	if err := m.Release("merge"); err != nil {
		t.Fatalf("Release: %v", err)
	}

	// Acquire should complete successfully.
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Acquire after Release: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Acquire did not complete after Release within timeout")
	}

	// Verify the waiter is now active.
	active, _, err := m.Status("merge")
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if active == nil {
		t.Fatal("expected active entry after Acquire")
	}
	if active.Branch != "feature/waiter" {
		t.Errorf("expected active branch=feature/waiter, got %q", active.Branch)
	}
}
