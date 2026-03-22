// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"fmt"
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

	notifID, err := mgr.NotifyStart("feature/test", "ws1", "test summary", "", nil, "", false)
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

	_, err := mgr.NotifyStart("feature/skip", "ws1", "skip design", "", nil, "", true)
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

	_, err := mgr.NotifyStart("feature/backlog-126", "ws1", "backlog item", "", []int{126}, "", false)
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

	_, err := mgr.NotifyStart("feature/trans", "ws1", "summary", "", nil, "", false)
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

	_, _ = mgr.NotifyStart("feature/plan", "ws1", "summary", "", nil, "", false)
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

	_, _ = mgr.NotifyStart("feature/merge", "ws1", "summary", "", nil, "", true) // skipDesign → implementing

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

	_, _ = mgr.NotifyStart("feature/invalid", "ws1", "summary", "", nil, "", false)

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
	_, _ = mgr.NotifyStart("feature/branchA", "ws1", "summary A", "", nil, "", false)

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
	_, _ = mgr.NotifyStart("feature/branchA", "ws1", "summary A", "", nil, "", false)

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
	notifID, _ := mgr.NotifyStart("feature/branchA", "ws1", "summary A", "", nil, "", false)

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

	notifID, _ := mgr.NotifyStart("feature/branchA", "ws1", "summary A", "", nil, "", false)

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

	_, _ = mgr.NotifyStart("feature/backlog-42", "ws1", "summary", "", []int{42}, "", false)

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

	_, _ = mgr.NotifyStart("feature/found", "ws2", "found summary", "", []int{7}, "", false)

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

	_, _ = mgr.NotifyStart("feature/present", "ws1", "summary", "", nil, "", false)

	status, err := mgr.GetStatus("feature/present")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != StatusStarted {
		t.Errorf("expected status %q, got %q", StatusStarted, status)
	}
}

// ── ResolveBranch ──

func TestResolveBranch_ByWorkspace(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// Register with workspace "branch_02"
	_, err := mgr.NotifyStart("branch_02", "ws1", "summary", "feature/test-branch", nil, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// Should find by workspace ID
	resolved, err := mgr.ResolveBranch("branch_02", "")
	if err != nil {
		t.Fatalf("ResolveBranch: %v", err)
	}
	if resolved != "branch_02" {
		t.Errorf("expected resolved=%q, got %q", "branch_02", resolved)
	}
}

func TestResolveBranch_FallbackGitBranch(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// Register with workspace "branch_02" and git_branch "feature/test-branch"
	_, err := mgr.NotifyStart("branch_02", "ws1", "summary", "feature/test-branch", nil, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// Query with a different workspace ID but correct git_branch → fallback
	resolved, err := mgr.ResolveBranch("branch_99", "feature/test-branch")
	if err != nil {
		t.Fatalf("ResolveBranch: %v", err)
	}
	if resolved != "branch_02" {
		t.Errorf("expected resolved=%q (via git_branch fallback), got %q", "branch_02", resolved)
	}
}

func TestResolveBranch_NotFound(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// No branches registered — both workspace and git_branch miss
	resolved, err := mgr.ResolveBranch("branch_99", "feature/nonexistent")
	if err != nil {
		t.Fatalf("ResolveBranch: %v", err)
	}
	if resolved != "" {
		t.Errorf("expected empty string for not found, got %q", resolved)
	}
}

// ── NotifyStart with BacklogOperator ──

// mockBacklogManager implements BacklogOperator for testing.
type mockBacklogManager struct {
	items         map[int]string // id → status
	failSetStatus bool
}

func (m *mockBacklogManager) Check(id int) (bool, string, error) {
	status, exists := m.items[id]
	if !exists {
		return false, "", nil
	}
	return true, status, nil
}

func (m *mockBacklogManager) SetStatus(id int, status string) error {
	if m.failSetStatus {
		return fmt.Errorf("mock SetStatus error for id=%d", id)
	}
	m.items[id] = status
	return nil
}

func (m *mockBacklogManager) SetStatusWith(txs *store.Store, id int, status string) error {
	if m.failSetStatus {
		return fmt.Errorf("mock SetStatusWith error for id=%d", id)
	}
	m.items[id] = status
	return nil
}

func (m *mockBacklogManager) ListFixingForBranch(branch string, backlogIDs []int) ([]int, error) {
	var fixing []int
	for _, id := range backlogIDs {
		if status, exists := m.items[id]; exists && status == "FIXING" {
			fixing = append(fixing, id)
		}
	}
	return fixing, nil
}

func setupHandoffTestDBWithBacklog(t *testing.T, bm BacklogOperator) (*store.Store, *Manager) {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	mig := store.NewMigrator(s)
	mod := New(s, bm)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	return s, mod.manager
}

func TestNotifyStart_BacklogSetStatusFails(t *testing.T) {
	bm := &mockBacklogManager{
		items:         map[int]string{100: "OPEN"},
		failSetStatus: true,
	}
	s, mgr := setupHandoffTestDBWithBacklog(t, bm)

	_, err := mgr.NotifyStart("feature/fail-backlog", "ws1", "summary", "", []int{100}, "", false)
	if err == nil {
		t.Fatal("expected error when SetStatusWith fails, got nil")
	}

	// Verify rollback: branch should NOT exist
	b, err := mgr.GetBranch("feature/fail-backlog")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b != nil {
		t.Errorf("expected branch to NOT exist after rollback, got %+v", b)
	}

	// Verify rollback: backlog status should remain OPEN (not FIXING)
	if bm.items[100] != "OPEN" {
		t.Errorf("expected backlog 100 to remain OPEN after rollback, got %q", bm.items[100])
	}

	// Verify no junction entry
	var count int
	row := s.QueryRow("SELECT COUNT(*) FROM branch_backlogs WHERE branch=?", "feature/fail-backlog")
	if err := row.Scan(&count); err != nil {
		t.Fatalf("query junction: %v", err)
	}
	if count != 0 {
		t.Errorf("expected 0 junction entries after rollback, got %d", count)
	}
}

func TestNotifyStart_BacklogAlreadyFixing(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{200: "FIXING"},
	}
	_, mgr := setupHandoffTestDBWithBacklog(t, bm)

	_, err := mgr.NotifyStart("feature/already-fixing", "ws1", "summary", "", []int{200}, "", false)
	if err == nil {
		t.Fatal("expected error when backlog is already FIXING, got nil")
	}

	// Branch should NOT be created
	b, err := mgr.GetBranch("feature/already-fixing")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b != nil {
		t.Errorf("expected branch to NOT exist, got %+v", b)
	}
}

func TestNotifyStart_ReplaceMergeNotified(t *testing.T) {
	s, mgr := setupHandoffTestDB(t)

	// 1. 첫 번째 작업 등록 → implementing → merge-notified
	_, err := mgr.NotifyStart("feature/ws-reuse", "ws1", "first work", "git-branch-1", nil, "", true)
	if err != nil {
		t.Fatalf("first NotifyStart: %v", err)
	}
	if _, err := mgr.NotifyTransition("feature/ws-reuse", "ws1", "merge", "merge done"); err != nil {
		t.Fatalf("NotifyTransition merge: %v", err)
	}

	// 상태 확인: MERGE_NOTIFIED
	status, _ := mgr.GetStatus("feature/ws-reuse")
	if status != StatusMergeNotified {
		t.Fatalf("expected %q, got %q", StatusMergeNotified, status)
	}

	// 2. 같은 workspace에서 새 작업 시작 — MERGE_NOTIFIED 자동 정리 후 등록
	notifID, err := mgr.NotifyStart("feature/ws-reuse", "ws1", "second work", "git-branch-2", nil, "", true)
	if err != nil {
		t.Fatalf("second NotifyStart should succeed but got: %v", err)
	}
	if notifID <= 0 {
		t.Fatalf("expected positive notification ID, got %d", notifID)
	}

	// 새 작업의 상태 확인
	b, err := mgr.GetBranch("feature/ws-reuse")
	if err != nil {
		t.Fatalf("GetBranch: %v", err)
	}
	if b == nil {
		t.Fatal("expected branch to exist")
	}
	if b.Status != StatusImplementing {
		t.Errorf("expected %q, got %q", StatusImplementing, b.Status)
	}
	if b.Summary != "second work" {
		t.Errorf("expected summary %q, got %q", "second work", b.Summary)
	}

	// 이전 notification은 정리되고 새 notification만 존재하는지 확인
	rows, err := s.Query(`SELECT COUNT(*) FROM notifications WHERE branch = ?`, "feature/ws-reuse")
	if err != nil {
		t.Fatalf("query notifications: %v", err)
	}
	defer rows.Close()
	rows.Next()
	var count int
	rows.Scan(&count)
	if count != 1 {
		t.Errorf("expected 1 notification (new start), got %d", count)
	}
}

func TestNotifyStart_ReplaceImplementing_AbandonedWork(t *testing.T) {
	s, _ := setupHandoffTestDB(t)

	// backlog 모듈 준비 (FIXING 복귀 테스트를 위해)
	_, err := s.Exec(`CREATE TABLE IF NOT EXISTS backlog_items (
		id INTEGER PRIMARY KEY, title TEXT, severity TEXT, timeframe TEXT,
		scope TEXT, type TEXT, description TEXT, related TEXT, status TEXT DEFAULT 'OPEN',
		created_at TEXT, updated_at TEXT
	)`)
	if err != nil {
		t.Fatalf("create backlog_items: %v", err)
	}
	_, err = s.Exec(`INSERT INTO backlog_items (id, title, status) VALUES (200, 'test backlog', 'OPEN')`)
	if err != nil {
		t.Fatalf("insert backlog: %v", err)
	}

	// mock backlog manager — SetStatusWith로 실제 DB 갱신
	mock := &mockBacklogManagerForReplace{store: s}
	mgrWithBacklog := NewManager(s, mock)

	// 1. 백로그 연결하여 첫 작업 시작 (FIXING으로 전이됨)
	_, err = mgrWithBacklog.NotifyStart("feature/abandon", "ws1", "first work", "", []int{200}, "", true)
	if err != nil {
		t.Fatalf("first NotifyStart: %v", err)
	}

	// backlog 200이 FIXING인지 확인
	var bStatus string
	s.QueryRow(`SELECT status FROM backlog_items WHERE id = 200`).Scan(&bStatus)
	if bStatus != "FIXING" {
		t.Fatalf("expected backlog status FIXING, got %q", bStatus)
	}

	// 2. 중도 포기 — 같은 branch에서 새 작업 시작 (FIXING 백로그 → OPEN 복귀)
	_, err = mgrWithBacklog.NotifyStart("feature/abandon", "ws1", "new work after abandon", "", nil, "", true)
	if err != nil {
		t.Fatalf("second NotifyStart should succeed (abandon+replace): %v", err)
	}

	// backlog 200이 OPEN으로 복귀했는지 확인
	s.QueryRow(`SELECT status FROM backlog_items WHERE id = 200`).Scan(&bStatus)
	if bStatus != "OPEN" {
		t.Errorf("expected backlog status OPEN after abandon, got %q", bStatus)
	}

	// 새 작업 상태 확인
	b, _ := mgrWithBacklog.GetBranch("feature/abandon")
	if b == nil {
		t.Fatal("expected branch to exist")
	}
	if b.Status != StatusImplementing {
		t.Errorf("expected %q, got %q", StatusImplementing, b.Status)
	}
	if b.Summary != "new work after abandon" {
		t.Errorf("expected summary %q, got %q", "new work after abandon", b.Summary)
	}
}

// mockBacklogManagerForReplace implements BacklogOperator with real DB operations.
type mockBacklogManagerForReplace struct {
	store *store.Store
}

func (m *mockBacklogManagerForReplace) SetStatus(id int, status string) error {
	_, err := m.store.Exec(`UPDATE backlog_items SET status = ? WHERE id = ?`, status, id)
	return err
}

func (m *mockBacklogManagerForReplace) SetStatusWith(txs *store.Store, id int, status string) error {
	_, err := txs.Exec(`UPDATE backlog_items SET status = ? WHERE id = ?`, status, id)
	return err
}

func (m *mockBacklogManagerForReplace) Check(id int) (bool, string, error) {
	var status string
	err := m.store.QueryRow(`SELECT status FROM backlog_items WHERE id = ?`, id).Scan(&status)
	if err != nil {
		return false, "", nil
	}
	return true, status, nil
}

func (m *mockBacklogManagerForReplace) ListFixingForBranch(branch string, backlogIDs []int) ([]int, error) {
	return nil, nil
}
