// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupHandoffTestDB(t *testing.T) (*store.Store, *Manager) {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	mig := store.NewMigrator(s)
	mod := New(s, nil)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	return s, mod.manager
}

func TestNotifyStart_Basic(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	notifID, err := mgr.NotifyStart("feature/test", "ws1", "test summary", nil, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}
	if notifID <= 0 {
		t.Fatalf("expected positive notification ID, got %d", notifID)
	}

	b, err := mgr.GetBranch("feature/test")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b == nil {
		t.Fatal("expected branch to exist")
	}
	if b.Status != StatusStarted {
		t.Errorf("expected status %q, got %q", StatusStarted, b.Status)
	}
	if b.Branch != "feature/test" {
		t.Errorf("expected branch %q, got %q", "feature/test", b.Branch)
	}
	if b.Workspace != "ws1" {
		t.Errorf("expected workspace %q, got %q", "ws1", b.Workspace)
	}
}

func TestNotifyStart_SkipDesign(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, err := mgr.NotifyStart("feature/skip", "ws1", "skip design", nil, "", true)
	if err != nil {
		t.Fatalf("NotifyStart with skipDesign: %v", err)
	}

	status, err := mgr.GetStatus("feature/skip")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != StatusImplementing {
		t.Errorf("expected status %q, got %q", StatusImplementing, status)
	}
}

func TestNotifyStart_WithBacklog(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, err := mgr.NotifyStart("feature/backlog-126", "ws1", "backlog item", []int{126}, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	b, err := mgr.GetBranch("feature/backlog-126")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b == nil {
		t.Fatal("expected branch to exist")
	}
	if len(b.BacklogIDs) != 1 || b.BacklogIDs[0] != 126 {
		t.Errorf("expected BacklogIDs [126], got %v", b.BacklogIDs)
	}
}

func TestNotifyTransition_Design(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, err := mgr.NotifyStart("feature/trans", "ws1", "summary", nil, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	notifID, err := mgr.NotifyTransition("feature/trans", "ws1", TypeDesign, "design summary")
	if err != nil {
		t.Fatalf("NotifyTransition design: %v", err)
	}
	if notifID <= 0 {
		t.Fatalf("expected positive notification ID, got %d", notifID)
	}

	status, err := mgr.GetStatus("feature/trans")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != StatusDesignNotified {
		t.Errorf("expected status %q, got %q", StatusDesignNotified, status)
	}
}

func TestNotifyTransition_Plan(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, _ = mgr.NotifyStart("feature/plan", "ws1", "summary", nil, "", false)
	_, _ = mgr.NotifyTransition("feature/plan", "ws1", TypeDesign, "design")

	_, err := mgr.NotifyTransition("feature/plan", "ws1", TypePlan, "plan summary")
	if err != nil {
		t.Fatalf("NotifyTransition plan: %v", err)
	}

	status, err := mgr.GetStatus("feature/plan")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != StatusImplementing {
		t.Errorf("expected status %q, got %q", StatusImplementing, status)
	}
}

func TestNotifyTransition_Merge(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, _ = mgr.NotifyStart("feature/merge", "ws1", "summary", nil, "", true) // skipDesign → implementing

	_, err := mgr.NotifyTransition("feature/merge", "ws1", TypeMerge, "merge summary")
	if err != nil {
		t.Fatalf("NotifyTransition merge: %v", err)
	}

	status, err := mgr.GetStatus("feature/merge")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != StatusMergeNotified {
		t.Errorf("expected status %q, got %q", StatusMergeNotified, status)
	}
}

func TestNotifyTransition_Invalid(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, _ = mgr.NotifyStart("feature/invalid", "ws1", "summary", nil, "", false)

	// started → plan is invalid (must go through design first)
	_, err := mgr.NotifyTransition("feature/invalid", "ws1", TypePlan, "skip design")
	if err == nil {
		t.Fatal("expected error for invalid transition started→plan, got nil")
	}
}

func TestCheckNotifications_None(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	notifs, err := mgr.CheckNotifications("feature/checker")
	if err != nil {
		t.Fatalf("CheckNotifications: %v", err)
	}
	if len(notifs) != 0 {
		t.Errorf("expected 0 notifications, got %d", len(notifs))
	}
}

func TestCheckNotifications_ExcludesOwn(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// Branch A creates a notification
	_, _ = mgr.NotifyStart("feature/branchA", "ws1", "summary A", nil, "", false)

	// Branch A checks — should NOT see its own notification
	notifs, err := mgr.CheckNotifications("feature/branchA")
	if err != nil {
		t.Fatalf("CheckNotifications: %v", err)
	}
	if len(notifs) != 0 {
		t.Errorf("expected 0 notifications (own excluded), got %d", len(notifs))
	}
}

func TestCheckNotifications_SeesOthers(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// Branch A creates a notification
	_, _ = mgr.NotifyStart("feature/branchA", "ws1", "summary A", nil, "", false)

	// Branch B checks — should see Branch A's notification
	notifs, err := mgr.CheckNotifications("feature/branchB")
	if err != nil {
		t.Fatalf("CheckNotifications: %v", err)
	}
	if len(notifs) == 0 {
		t.Fatal("expected branch B to see branch A's notification")
	}
	if notifs[0].Branch != "feature/branchA" {
		t.Errorf("expected notification from branchA, got %q", notifs[0].Branch)
	}
}

func TestCheckNotifications_ExcludesAcked(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// Branch A creates a notification
	notifID, _ := mgr.NotifyStart("feature/branchA", "ws1", "summary A", nil, "", false)

	// Branch B acks it
	err := mgr.Ack(notifID, "feature/branchB", "no-impact")
	if err != nil {
		t.Fatalf("Ack: %v", err)
	}

	// Branch B checks — acked notification should be excluded
	notifs, err := mgr.CheckNotifications("feature/branchB")
	if err != nil {
		t.Fatalf("CheckNotifications after ack: %v", err)
	}
	if len(notifs) != 0 {
		t.Errorf("expected 0 notifications after ack, got %d", len(notifs))
	}
}

func TestAck_Basic(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	notifID, _ := mgr.NotifyStart("feature/branchA", "ws1", "summary A", nil, "", false)

	err := mgr.Ack(notifID, "feature/branchB", "no-impact")
	if err != nil {
		t.Fatalf("Ack: %v", err)
	}

	// Verify in notification_acks
	var count int
	row := mgr.store.QueryRow(
		"SELECT COUNT(*) FROM notification_acks WHERE notification_id=? AND branch=? AND action=?",
		notifID, "feature/branchB", "no-impact",
	)
	if err := row.Scan(&count); err != nil {
		t.Fatalf("query acks: %v", err)
	}
	if count != 1 {
		t.Errorf("expected 1 ack record, got %d", count)
	}
}

func TestBacklogCheck_Available(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	available, branch, err := mgr.BacklogCheck(999)
	if err != nil {
		t.Fatalf("BacklogCheck: %v", err)
	}
	if !available {
		t.Errorf("expected available=true, got false (branch: %q)", branch)
	}
	if branch != "" {
		t.Errorf("expected empty branch, got %q", branch)
	}
}

func TestBacklogCheck_InUse(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, _ = mgr.NotifyStart("feature/backlog-42", "ws1", "summary", []int{42}, "", false)

	available, branch, err := mgr.BacklogCheck(42)
	if err != nil {
		t.Fatalf("BacklogCheck: %v", err)
	}
	if available {
		t.Error("expected available=false, got true")
	}
	if branch != "feature/backlog-42" {
		t.Errorf("expected branch %q, got %q", "feature/backlog-42", branch)
	}
}

func TestGetBranch_NotFound(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	b, err := mgr.GetBranch("feature/nonexistent")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b != nil {
		t.Errorf("expected nil branch, got %+v", b)
	}
}

func TestGetBranch_Found(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, _ = mgr.NotifyStart("feature/found", "ws2", "found summary", []int{7}, "", false)

	b, err := mgr.GetBranch("feature/found")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b == nil {
		t.Fatal("expected branch, got nil")
	}
	if b.Branch != "feature/found" {
		t.Errorf("branch mismatch: got %q", b.Branch)
	}
	if b.Workspace != "ws2" {
		t.Errorf("workspace mismatch: got %q", b.Workspace)
	}
	if b.Summary != "found summary" {
		t.Errorf("summary mismatch: got %q", b.Summary)
	}
	if len(b.BacklogIDs) != 1 || b.BacklogIDs[0] != 7 {
		t.Errorf("backlog_ids mismatch: got %v", b.BacklogIDs)
	}
	if b.Status != StatusStarted {
		t.Errorf("status mismatch: got %q", b.Status)
	}
}

func TestGetStatus_NotRegistered(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	status, err := mgr.GetStatus("feature/ghost")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != "" {
		t.Errorf("expected empty status, got %q", status)
	}
}

func TestGetStatus_Registered(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_, _ = mgr.NotifyStart("feature/present", "ws1", "summary", nil, "", false)

	status, err := mgr.GetStatus("feature/present")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != StatusStarted {
		t.Errorf("expected status %q, got %q", StatusStarted, status)
	}
}
