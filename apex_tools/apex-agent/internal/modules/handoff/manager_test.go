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

	err := mgr.NotifyStart("feature/test", "ws1", "test summary", "", nil, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
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

	err := mgr.NotifyStart("feature/skip", "ws1", "skip design", "", nil, "", true)
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

	err := mgr.NotifyStart("feature/backlog-126", "ws1", "backlog item", "", []int{126}, "", false)
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

	err := mgr.NotifyStart("feature/trans", "ws1", "summary", "", nil, "", false)
	if err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	if err = mgr.NotifyTransition("feature/trans", "ws1", TypeDesign, "design summary"); err != nil {
		t.Fatalf("NotifyTransition design: %v", err)
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

	_ = mgr.NotifyStart("feature/plan", "ws1", "summary", "", nil, "", false)
	_ = mgr.NotifyTransition("feature/plan", "ws1", TypeDesign, "design")

	err := mgr.NotifyTransition("feature/plan", "ws1", TypePlan, "plan summary")
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

func TestNotifyMerge(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_ = mgr.NotifyStart("feature/merge", "ws1", "summary", "", nil, "", true) // skipDesign → implementing

	err := mgr.NotifyMerge("feature/merge", "ws1", "merge summary")
	if err != nil {
		t.Fatalf("NotifyMerge: %v", err)
	}

	// 머지 후 active_branches에서 사라져야 함
	status, err := mgr.GetStatus("feature/merge")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != "" {
		t.Errorf("expected empty status (removed from active), got %q", status)
	}
}

func TestNotifyTransition_Invalid(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	_ = mgr.NotifyStart("feature/invalid", "ws1", "summary", "", nil, "", false)

	// started → plan is invalid (must go through design first)
	err := mgr.NotifyTransition("feature/invalid", "ws1", TypePlan, "skip design")
	if err == nil {
		t.Fatal("expected error for invalid transition started→plan, got nil")
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

	_ = mgr.NotifyStart("feature/backlog-42", "ws1", "summary", "", []int{42}, "", false)

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

	_ = mgr.NotifyStart("feature/found", "ws2", "found summary", "", []int{7}, "", false)

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

	_ = mgr.NotifyStart("feature/present", "ws1", "summary", "", nil, "", false)

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
	err := mgr.NotifyStart("branch_02", "ws1", "summary", "feature/test-branch", nil, "", false)
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
	err := mgr.NotifyStart("branch_02", "ws1", "summary", "feature/test-branch", nil, "", false)
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

func (m *mockBacklogManager) SetStatusWith(q store.Querier, id int, status string) error {
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

	err := mgr.NotifyStart("feature/fail-backlog", "ws1", "summary", "", []int{100}, "", false)
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

	err := mgr.NotifyStart("feature/already-fixing", "ws1", "summary", "", []int{200}, "", false)
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

func TestNotifyStart_AfterMerge(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// 1. 첫 번째 작업 등록 → implementing → merge
	err := mgr.NotifyStart("feature/ws-reuse", "ws1", "first work", "git-branch-1", nil, "", true)
	if err != nil {
		t.Fatalf("first NotifyStart: %v", err)
	}
	if err := mgr.NotifyMerge("feature/ws-reuse", "ws1", "merge done"); err != nil {
		t.Fatalf("NotifyMerge: %v", err)
	}

	// 머지 후 active에서 사라졌는지 확인
	status, _ := mgr.GetStatus("feature/ws-reuse")
	if status != "" {
		t.Fatalf("expected empty status after merge, got %q", status)
	}

	// 2. 같은 workspace에서 새 작업 시작 — 정상 등록
	err = mgr.NotifyStart("feature/ws-reuse", "ws1", "second work", "git-branch-2", nil, "", true)
	if err != nil {
		t.Fatalf("second NotifyStart should succeed but got: %v", err)
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
	err = mgrWithBacklog.NotifyStart("feature/abandon", "ws1", "first work", "", []int{200}, "", true)
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
	err = mgrWithBacklog.NotifyStart("feature/abandon", "ws1", "new work after abandon", "", nil, "", true)
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

func (m *mockBacklogManagerForReplace) SetStatusWith(q store.Querier, id int, status string) error {
	_, err := q.Exec(`UPDATE backlog_items SET status = ? WHERE id = ?`, status, id)
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

// ── NotifyDrop ──

func TestNotifyDrop_OK(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// implementing 상태로 등록
	if err := mgr.NotifyStart("feature/drop-ok", "ws1", "drop test", "git-drop", nil, "", true); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// drop 실행
	if err := mgr.NotifyDrop("feature/drop-ok", "ws1", "no longer needed"); err != nil {
		t.Fatalf("NotifyDrop: %v", err)
	}

	// active_branches에서 사라져야 함
	status, err := mgr.GetStatus("feature/drop-ok")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != "" {
		t.Errorf("expected empty status (removed from active), got %q", status)
	}

	// branch_history에 DROPPED로 기록되어야 함
	var historyStatus, historySummary string
	row := mgr.store.QueryRow(
		`SELECT status, summary FROM branch_history WHERE branch = ?`, "feature/drop-ok",
	)
	if err := row.Scan(&historyStatus, &historySummary); err != nil {
		t.Fatalf("scan branch_history: %v", err)
	}
	if historyStatus != HistoryDropped {
		t.Errorf("expected history status %q, got %q", HistoryDropped, historyStatus)
	}
	if historySummary != "no longer needed" {
		t.Errorf("expected history summary %q, got %q", "no longer needed", historySummary)
	}
}

func TestNotifyDrop_WithFixingBacklogs(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{50: "OPEN"},
	}
	_, mgr := setupHandoffTestDBWithBacklog(t, bm)

	// 백로그 50 연결하여 등록 → FIXING 전이
	if err := mgr.NotifyStart("feature/drop-fixing", "ws1", "has fixing", "", []int{50}, "", true); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// 백로그 50이 FIXING인지 확인
	if bm.items[50] != "FIXING" {
		t.Fatalf("expected backlog 50 FIXING, got %q", bm.items[50])
	}

	// drop 시도 → FIXING 백로그 때문에 차단되어야 함
	err := mgr.NotifyDrop("feature/drop-fixing", "ws1", "try drop")
	if err == nil {
		t.Fatal("expected error due to FIXING backlog, got nil")
	}

	// 브랜치는 여전히 active여야 함
	status, getErr := mgr.GetStatus("feature/drop-fixing")
	if getErr != nil {
		t.Fatalf("GetStatus: %v", getErr)
	}
	if status != StatusImplementing {
		t.Errorf("expected status %q (still active), got %q", StatusImplementing, status)
	}
}

func TestNotifyDrop_NotFound(t *testing.T) {
	_, mgr := setupHandoffTestDB(t)

	// 미등록 브랜치에서 drop 시도 → 에러
	err := mgr.NotifyDrop("feature/nonexistent", "ws1", "reason")
	if err == nil {
		t.Fatal("expected error for unregistered branch, got nil")
	}
}

func TestNotifyDrop_BacklogRelease(t *testing.T) {
	bm := &mockBacklogManager{
		items: map[int]string{60: "OPEN", 70: "OPEN"},
	}
	_, mgr := setupHandoffTestDBWithBacklog(t, bm)

	// 백로그 60, 70 연결하여 등록 → FIXING 전이
	if err := mgr.NotifyStart("feature/drop-release", "ws1", "release test", "", []int{60, 70}, "", true); err != nil {
		t.Fatalf("NotifyStart: %v", err)
	}

	// 둘 다 FIXING인지 확인
	if bm.items[60] != "FIXING" || bm.items[70] != "FIXING" {
		t.Fatalf("expected both backlogs FIXING, got 60=%q 70=%q", bm.items[60], bm.items[70])
	}

	// 백로그를 resolve/release 처리하여 FIXING 해제
	bm.items[60] = "RESOLVED"
	bm.items[70] = "OPEN"

	// FIXING이 없으므로 drop 성공해야 함
	if err := mgr.NotifyDrop("feature/drop-release", "ws1", "all backlogs cleared"); err != nil {
		t.Fatalf("NotifyDrop: %v", err)
	}

	// active에서 사라져야 함
	status, err := mgr.GetStatus("feature/drop-release")
	if err != nil {
		t.Fatalf("GetStatus: %v", err)
	}
	if status != "" {
		t.Errorf("expected empty status after drop, got %q", status)
	}

	// history에 DROPPED 기록
	var historyStatus string
	row := mgr.store.QueryRow(
		`SELECT status FROM branch_history WHERE branch = ?`, "feature/drop-release",
	)
	if err := row.Scan(&historyStatus); err != nil {
		t.Fatalf("scan branch_history: %v", err)
	}
	if historyStatus != HistoryDropped {
		t.Errorf("expected history status %q, got %q", HistoryDropped, historyStatus)
	}
}
