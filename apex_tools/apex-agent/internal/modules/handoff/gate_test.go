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
	_, mgr := setupGateTestDB(t)

	err := mgr.ValidateCommit(context.Background(), "branch_01")
	if err == nil {
		t.Error("unregistered branch should be blocked")
	}
}

func TestValidateCommit_Registered(t *testing.T) {
	_, mgr := setupGateTestDB(t)

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateCommit(context.Background(), "branch_01")
	if err != nil {
		t.Errorf("registered branch should be allowed: %v", err)
	}
}

// ── ValidateMergeGate ──

func TestValidateMergeGate_NoNotifications(t *testing.T) {
	_, mgr := setupGateTestDB(t)

	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateMergeGate(context.Background(), "branch_01")
	if err != nil {
		t.Errorf("no notifications should pass: %v", err)
	}
}

// ── ValidateEdit ──

func TestValidateEdit_NotRegistered(t *testing.T) {
	_, mgr := setupGateTestDB(t)

	err := mgr.ValidateEdit(context.Background(), "branch_01", "server.cpp")
	if err == nil {
		t.Error("unregistered should block")
	}
}

func TestValidateEdit_StatusGate(t *testing.T) {
	tests := []struct {
		name        string
		skipDesign  bool
		transition  string // "" = none, "design" = NotifyTransition(design)
		filePath    string
		wantErr     bool
	}{
		{"StartedBlocksSource", false, "", "server.cpp", true},
		{"StartedAllowsDocs", false, "", "docs/plan.md", false},
		{"DesignNotifiedBlocksSource", false, "design", "server.hpp", true},
		{"ImplementingAllowsAll", true, "", "server.cpp", false},
		{"ImplementingAllowsGoSource", true, "", "internal/modules/handoff/gate.go", false},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, mgr := setupGateTestDB(t)

			if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", tc.skipDesign); err != nil {
				t.Fatalf("NotifyStart: %v", err)
			}
			if tc.transition != "" {
				if err := mgr.NotifyTransition(context.Background(), "branch_01", "ws1", tc.transition, tc.transition+" summary"); err != nil {
					t.Fatalf("NotifyTransition: %v", err)
				}
			}

			err := mgr.ValidateEdit(context.Background(), "branch_01", tc.filePath)
			if tc.wantErr && err == nil {
				t.Errorf("expected error for %s with file %q, got nil", tc.name, tc.filePath)
			}
			if !tc.wantErr && err != nil {
				t.Errorf("expected no error for %s with file %q, got: %v", tc.name, tc.filePath, err)
			}
		})
	}
}

// ── ValidateMergeGate with FIXING backlog ──

func TestValidateMergeGate_FixingBacklogBlocks(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{42: "OPEN"},
	}
	_, mgr := setupGateTestDBWithMock(t, bm)

	// Register branch with backlog 42 — NotifyStart transitions it to FIXING
	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", []int{42}, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// Verify mock transitioned to FIXING
	if bm.items[42] != "FIXING" {
		t.Fatalf("expected backlog 42 to be FIXING after NotifyStart, got %q", bm.items[42])
	}

	// Merge gate should block because of FIXING backlog
	err := mgr.ValidateMergeGate(context.Background(), "branch_01")
	if err == nil {
		t.Error("expected merge to be blocked with FIXING backlog, got nil")
	}
}

func TestValidateMergeGate_NoFixingBacklogPasses(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{42: "RESOLVED"},
	}
	s, mgr := setupGateTestDBWithMock(t, bm)

	// Register branch without backlog IDs
	if err := mgr.NotifyStart(context.Background(), "branch_01", "ws1", "test", "", nil, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// Manually insert junction entry to simulate backlog association
	_, err := s.Exec(context.Background(), `INSERT INTO branch_backlogs (branch, backlog_id) VALUES (?, ?)`, "branch_01", 42)
	if err != nil {
		t.Fatalf("insert branch_backlog: %v", err)
	}

	// Merge gate should pass — backlog is RESOLVED, not FIXING
	err = mgr.ValidateMergeGate(context.Background(), "branch_01")
	if err != nil {
		t.Errorf("expected merge to pass with RESOLVED backlog, got: %v", err)
	}
}
