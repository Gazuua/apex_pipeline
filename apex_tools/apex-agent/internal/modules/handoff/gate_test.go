// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupGateTestDB(t *testing.T) (*store.Store, *Manager) {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	mig := store.NewMigrator(s)
	mod := New(s, nil)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	return s, mod.manager
}

// setupGateTestDBWithMock sets up handoff with a mock BacklogOperator for merge gate tests.
func setupGateTestDBWithMock(t *testing.T, bm BacklogOperator) (*store.Store, *Manager) {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	mig := store.NewMigrator(s)
	mod := New(s, bm)
	mod.RegisterSchema(mig)

	if err := mig.Migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	return s, mod.manager
}

// ── ValidateCommit ──

func TestValidateCommit_NotRegistered(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	err := mgr.ValidateCommit("branch_01")
	if err == nil {
		t.Error("unregistered branch should be blocked")
	}
}

func TestValidateCommit_Registered(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateCommit("branch_01")
	if err != nil {
		t.Errorf("registered branch should be allowed: %v", err)
	}
}

// ── ValidateMergeGate ──

func TestValidateMergeGate_NoNotifications(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateMergeGate("branch_01")
	if err != nil {
		t.Errorf("no notifications should pass: %v", err)
	}
}

// ── ValidateEdit ──

func TestValidateEdit_NotRegistered(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	err := mgr.ValidateEdit("branch_01", "server.cpp")
	if err == nil {
		t.Error("unregistered should block")
	}
}

func TestValidateEdit_StartedBlocksSource(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateEdit("branch_01", "server.cpp")
	if err == nil {
		t.Error("started should block source files")
	}
}

func TestValidateEdit_StartedAllowsDocs(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateEdit("branch_01", "docs/plan.md")
	if err != nil {
		t.Errorf("started should allow docs: %v", err)
	}
}

func TestValidateEdit_DesignNotifiedBlocksSource(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	if err := mgr.NotifyTransition(context.Background(), "branch_01", "ws1", "design", "design summary"); err != nil {
		t.Fatalf("NotifyTransition: %v", err)
	}
	err := mgr.ValidateEdit("branch_01", "server.hpp")
	if err == nil {
		t.Error("design-notified should block source files")
	}
}

func TestValidateEdit_ImplementingAllowsAll(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	// skip-design → implementing
	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", true); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateEdit("branch_01", "server.cpp")
	if err != nil {
		t.Errorf("implementing should allow source: %v", err)
	}
}

func TestValidateEdit_ImplementingAllowsGoSource(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", true); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateEdit("branch_01", "internal/modules/handoff/gate.go")
	if err != nil {
		t.Errorf("implementing should allow .go source: %v", err)
	}
}

// ── ValidateMergeGate with FIXING backlog ──

func TestValidateMergeGate_FixingBacklogBlocks(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{42: "OPEN"},
	}
	s, mgr := setupGateTestDBWithMock(t, bm)
	defer s.Close()

	// Register branch with backlog 42 — NotifyStart transitions it to FIXING
	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", []int{42}, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// Verify mock transitioned to FIXING
	if bm.items[42] != "FIXING" {
		t.Fatalf("expected backlog 42 to be FIXING after NotifyStart, got %q", bm.items[42])
	}

	// Merge gate should block because of FIXING backlog
	err := mgr.ValidateMergeGate("branch_01")
	if err == nil {
		t.Error("expected merge to be blocked with FIXING backlog, got nil")
	}
}

func TestValidateMergeGate_NoFixingBacklogPasses(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{42: "RESOLVED"},
	}
	s, mgr := setupGateTestDBWithMock(t, bm)
	defer s.Close()

	// Register branch without backlog IDs
	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// Manually insert junction entry to simulate backlog association
	_, err := s.Exec(`INSERT INTO branch_backlogs (branch, backlog_id) VALUES (?, ?)`, "branch_01", 42)
	if err != nil {
		t.Fatalf("insert branch_backlog: %v", err)
	}

	// Merge gate should pass — backlog is RESOLVED, not FIXING
	err = mgr.ValidateMergeGate("branch_01")
	if err != nil {
		t.Errorf("expected merge to pass with RESOLVED backlog, got: %v", err)
	}
}
