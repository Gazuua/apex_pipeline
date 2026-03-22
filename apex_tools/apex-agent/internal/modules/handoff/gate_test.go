// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupGateTestDB(t *testing.T) (*store.Store, *Manager) {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	mig := store.NewMigrator(s)
	mod := New(s)
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

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
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

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateMergeGate("branch_01")
	if err != nil {
		t.Errorf("no notifications should pass: %v", err)
	}
}

func TestValidateMergeGate_UnackedBlocks(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if _, err := mgr.NotifyStart("branch_01", "ws1", "work on branch 1", 0, "", false); err != nil {
		t.Fatalf("NotifyStart branch_01: %v", err)
	}
	if _, err := mgr.NotifyStart("branch_02", "ws2", "work on branch 2", 0, "", false); err != nil {
		t.Fatalf("NotifyStart branch_02: %v", err)
	}
	// branch_01 has an unacked notification from branch_02
	err := mgr.ValidateMergeGate("branch_01")
	if err == nil {
		t.Error("unacked notifications should block merge")
	}
}

func TestValidateMergeGate_AckedPasses(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
		t.Fatalf("NotifyStart branch_01: %v", err)
	}
	id, err := mgr.NotifyStart("branch_02", "ws2", "test2", 0, "", false)
	if err != nil {
		t.Fatalf("NotifyStart branch_02: %v", err)
	}
	if err := mgr.Ack(id, "branch_01", "no-impact"); err != nil {
		t.Fatalf("Ack: %v", err)
	}

	err = mgr.ValidateMergeGate("branch_01")
	if err != nil {
		t.Errorf("all acked should pass: %v", err)
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

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
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

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
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

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	if _, err := mgr.NotifyTransition("branch_01", "ws1", "design", "design summary"); err != nil {
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
	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", true); err != nil {
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

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", true); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	err := mgr.ValidateEdit("branch_01", "internal/modules/handoff/gate.go")
	if err != nil {
		t.Errorf("implementing should allow .go source: %v", err)
	}
}

// ── ProbeNotifications ──

func TestProbeNotifications_None(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	msg, err := mgr.ProbeNotifications("branch_01")
	if err != nil {
		t.Fatal(err)
	}
	if msg != "" {
		t.Errorf("no notifications should return empty, got: %s", msg)
	}
}

func TestProbeNotifications_HasNew(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
		t.Fatalf("NotifyStart branch_01: %v", err)
	}
	if _, err := mgr.NotifyStart("branch_02", "ws2", "new work", 0, "", false); err != nil {
		t.Fatalf("NotifyStart branch_02: %v", err)
	}

	msg, err := mgr.ProbeNotifications("branch_01")
	if err != nil {
		t.Fatal(err)
	}
	if msg == "" {
		t.Error("should have notification message")
	}
}

func TestProbeNotifications_AckedNotShown(t *testing.T) {
	s, mgr := setupGateTestDB(t)
	defer s.Close()

	if _, err := mgr.NotifyStart("branch_01", "ws1", "test", 0, "", false); err != nil {
		t.Fatalf("NotifyStart branch_01: %v", err)
	}
	id, err := mgr.NotifyStart("branch_02", "ws2", "new work", 0, "", false)
	if err != nil {
		t.Fatalf("NotifyStart branch_02: %v", err)
	}
	if err := mgr.Ack(id, "branch_01", "no-impact"); err != nil {
		t.Fatalf("Ack: %v", err)
	}

	msg, err := mgr.ProbeNotifications("branch_01")
	if err != nil {
		t.Fatal(err)
	}
	if msg != "" {
		t.Errorf("acked notification should not show, got: %s", msg)
	}
}
