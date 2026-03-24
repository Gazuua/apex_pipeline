// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"fmt"
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// ── Test helpers: minimal querier implementations backed by store ──

type testBacklogQuerier struct {
	st *store.Store
}

func (q *testBacklogQuerier) DashboardStatusCounts() (map[string]int, error) {
	rows, err := q.st.Query(`SELECT status, COUNT(*) FROM backlog_items GROUP BY status`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	counts := make(map[string]int)
	for rows.Next() {
		var s string
		var c int
		if err := rows.Scan(&s, &c); err != nil {
			return nil, err
		}
		counts[s] = c
	}
	return counts, rows.Err()
}

func (q *testBacklogQuerier) DashboardSeverityCounts() (map[string]int, error) {
	rows, err := q.st.Query(`SELECT severity, COUNT(*) FROM backlog_items WHERE status != 'RESOLVED' GROUP BY severity`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	counts := make(map[string]int)
	for rows.Next() {
		var s string
		var c int
		if err := rows.Scan(&s, &c); err != nil {
			return nil, err
		}
		counts[s] = c
	}
	return counts, rows.Err()
}

func (q *testBacklogQuerier) DashboardListItems(f BacklogFilter) ([]BacklogItem, error) {
	var where []string
	var args []any

	addInFilter := func(col string, vals []string) {
		if len(vals) == 0 {
			return
		}
		placeholders := make([]string, len(vals))
		for i, v := range vals {
			placeholders[i] = "?"
			args = append(args, v)
		}
		where = append(where, col+" IN ("+strings.Join(placeholders, ",")+")")
	}
	addInFilter("status", f.Status)
	addInFilter("severity", f.Severity)
	addInFilter("timeframe", f.Timeframe)
	addInFilter("scope", f.Scope)
	addInFilter("type", f.Type)

	query := `SELECT id, title, severity, timeframe, scope, type, status, description,
	          COALESCE(related,''), COALESCE(resolution,''), created_at, updated_at
	          FROM backlog_items`
	if len(where) > 0 {
		query += " WHERE " + strings.Join(where, " AND ")
	}

	if f.SortBy != "" {
		sortCol := "id"
		allowed := map[string]bool{"id": true, "severity": true, "created_at": true, "updated_at": true, "status": true}
		if allowed[f.SortBy] {
			sortCol = f.SortBy
		}
		sortDir := "DESC"
		if f.SortDir == "ASC" {
			sortDir = "ASC"
		}
		query += " ORDER BY " + sortCol + " " + sortDir
	} else {
		query += ` ORDER BY
			CASE status WHEN 'FIXING' THEN 0 WHEN 'OPEN' THEN 1 ELSE 2 END,
			CASE timeframe WHEN 'NOW' THEN 0 WHEN 'IN_VIEW' THEN 1 WHEN 'DEFERRED' THEN 2 ELSE 3 END,
			CASE severity WHEN 'CRITICAL' THEN 0 WHEN 'MAJOR' THEN 1 WHEN 'MINOR' THEN 2 ELSE 3 END,
			id DESC`
	}

	rows, err := q.st.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var items []BacklogItem
	for rows.Next() {
		var b BacklogItem
		if err := rows.Scan(&b.ID, &b.Title, &b.Severity, &b.Timeframe, &b.Scope, &b.Type,
			&b.Status, &b.Description, &b.Related, &b.Resolution, &b.CreatedAt, &b.UpdatedAt); err != nil {
			return nil, err
		}
		items = append(items, b)
	}
	return items, rows.Err()
}

func (q *testBacklogQuerier) DashboardGetItemByID(id int) (*BacklogItem, error) {
	return nil, nil // not tested here
}

type testHandoffQuerier struct {
	st *store.Store
}

func (q *testHandoffQuerier) DashboardActiveBranchesList() ([]ActiveBranch, error) {
	rows, err := q.st.Query(`
		SELECT branch, workspace, COALESCE(git_branch,''), COALESCE(summary,''),
		       status, created_at
		FROM active_branches ORDER BY created_at DESC`)
	if err != nil {
		return nil, err
	}
	var branches []ActiveBranch
	for rows.Next() {
		var ab ActiveBranch
		if err := rows.Scan(&ab.Branch, &ab.WorkspaceID, &ab.GitBranch, &ab.Summary,
			&ab.Status, &ab.CreatedAt); err != nil {
			rows.Close()
			return nil, err
		}
		branches = append(branches, ab)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()

	for i := range branches {
		bbRows, err := q.st.Query(`SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branches[i].Branch)
		if err != nil {
			continue
		}
		for bbRows.Next() {
			var bid int
			if scanErr := bbRows.Scan(&bid); scanErr != nil {
				continue
			}
			branches[i].BacklogIDs = append(branches[i].BacklogIDs, bid)
		}
		bbRows.Close()
	}
	return branches, nil
}

func (q *testHandoffQuerier) DashboardActiveCount() (int, error) {
	var count int
	if err := q.st.QueryRow(`SELECT COUNT(*) FROM active_branches`).Scan(&count); err != nil {
		return 0, err
	}
	return count, nil
}

func (q *testHandoffQuerier) DashboardBranchHistoryList(limit int) ([]BranchHistory, error) {
	rows, err := q.st.Query(`
		SELECT id, branch, COALESCE(git_branch,''), COALESCE(summary,''),
		       status, COALESCE(backlog_ids,''), started_at, finished_at
		FROM branch_history ORDER BY finished_at DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var history []BranchHistory
	for rows.Next() {
		var bh BranchHistory
		if err := rows.Scan(&bh.ID, &bh.Branch, &bh.GitBranch, &bh.Summary,
			&bh.Status, &bh.BacklogIDs, &bh.StartedAt, &bh.FinishedAt); err != nil {
			return nil, err
		}
		history = append(history, bh)
	}
	return history, rows.Err()
}

type testQueueQuerier struct {
	st *store.Store
}

func (q *testQueueQuerier) DashboardQueueAll() ([]QueueEntry, error) {
	rows, err := q.st.Query(`
		SELECT channel, branch, status, created_at, COALESCE(finished_at,''),
		       CASE WHEN finished_at IS NOT NULL
		            THEN CAST((julianday(finished_at) - julianday(created_at)) * 86400 AS INTEGER)
		            ELSE 0 END as duration_sec
		FROM queue ORDER BY channel, created_at DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var entries []QueueEntry
	for rows.Next() {
		var e QueueEntry
		var durSec int
		if err := rows.Scan(&e.Channel, &e.Branch, &e.Status, &e.CreatedAt, &e.FinishedAt, &durSec); err != nil {
			return nil, err
		}
		if durSec > 0 {
			h := durSec / 3600
			m := (durSec % 3600) / 60
			s := durSec % 60
			if h > 0 {
				e.Duration = fmt.Sprintf("%dh %dm %ds", h, m, s)
			} else if m > 0 {
				e.Duration = fmt.Sprintf("%dm %ds", m, s)
			} else {
				e.Duration = fmt.Sprintf("%ds", s)
			}
		}
		entries = append(entries, e)
	}
	return entries, rows.Err()
}

func (q *testQueueQuerier) DashboardLockStatus(channel string) (bool, error) {
	var count int
	if err := q.st.QueryRow(
		`SELECT COUNT(*) FROM queue WHERE channel=? AND status='ACTIVE'`, channel,
	).Scan(&count); err != nil {
		return false, err
	}
	return count > 0, nil
}

// ── Setup ──

func setupTestStore(t *testing.T) *store.Store {
	t.Helper()
	st, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { st.Close() })

	tables := []string{
		`CREATE TABLE IF NOT EXISTS backlog_items (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			title TEXT NOT NULL, severity TEXT NOT NULL, timeframe TEXT NOT NULL,
			scope TEXT NOT NULL, type TEXT NOT NULL, description TEXT NOT NULL,
			related TEXT, position INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'OPEN',
			resolution TEXT, resolved_at TEXT,
			created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`,
		`CREATE TABLE IF NOT EXISTS active_branches (
			branch TEXT PRIMARY KEY, workspace TEXT NOT NULL, git_branch TEXT UNIQUE,
			status TEXT NOT NULL, summary TEXT,
			created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`,
		`CREATE TABLE IF NOT EXISTS branch_backlogs (
			branch TEXT NOT NULL, backlog_id INTEGER NOT NULL,
			PRIMARY KEY (branch, backlog_id)
		)`,
		`CREATE TABLE IF NOT EXISTS branch_history (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			branch TEXT NOT NULL, workspace TEXT NOT NULL, git_branch TEXT,
			status TEXT NOT NULL, summary TEXT, backlog_ids TEXT,
			started_at TEXT NOT NULL,
			finished_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`,
		`CREATE TABLE IF NOT EXISTS queue (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			channel TEXT NOT NULL, branch TEXT NOT NULL, pid INTEGER NOT NULL,
			status TEXT NOT NULL DEFAULT 'WAITING',
			created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
			finished_at TEXT
		)`,
	}
	for _, ddl := range tables {
		if _, err := st.Exec(ddl); err != nil {
			t.Fatalf("create table: %v", err)
		}
	}
	return st
}

func setupTestQueriers(t *testing.T) (*store.Store, BacklogQuerier, HandoffQuerier, QueueQuerier) {
	t.Helper()
	st := setupTestStore(t)
	return st, &testBacklogQuerier{st}, &testHandoffQuerier{st}, &testQueueQuerier{st}
}

// ── Tests ──

func TestQueryDashboardSummary_Empty(t *testing.T) {
	_, bm, hm, qm := setupTestQueriers(t)
	s, err := queryDashboardSummary(bm, hm, qm)
	if err != nil {
		t.Fatalf("queryDashboardSummary: %v", err)
	}
	if s.BacklogOpen != 0 || s.BacklogFixing != 0 || s.BacklogResolved != 0 {
		t.Errorf("expected all 0, got %+v", s)
	}
	if s.ActiveBranches != 0 {
		t.Errorf("expected 0 active branches, got %d", s.ActiveBranches)
	}
}

func TestQueryDashboardSummary_WithData(t *testing.T) {
	st, bm, hm, qm := setupTestQueriers(t)

	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('bug1','CRITICAL','NOW','CORE','BUG','desc',1,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('bug2','MAJOR','NOW','CORE','BUG','desc',2,'FIXING')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('bug3','MINOR','IN_VIEW','SHARED','BUG','desc',3,'RESOLVED')`)

	s, err := queryDashboardSummary(bm, hm, qm)
	if err != nil {
		t.Fatalf("queryDashboardSummary: %v", err)
	}
	if s.BacklogOpen != 1 {
		t.Errorf("expected 1 open, got %d", s.BacklogOpen)
	}
	if s.BacklogFixing != 1 {
		t.Errorf("expected 1 fixing, got %d", s.BacklogFixing)
	}
	if s.BacklogResolved != 1 {
		t.Errorf("expected 1 resolved, got %d", s.BacklogResolved)
	}
	if s.CriticalCount != 1 {
		t.Errorf("expected 1 critical, got %d", s.CriticalCount)
	}
	if s.MajorCount != 1 {
		t.Errorf("expected 1 major, got %d", s.MajorCount)
	}
}

func TestQueryBacklogList_Filter(t *testing.T) {
	st, bm, _, _ := setupTestQueriers(t)

	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('a','CRITICAL','NOW','CORE','BUG','desc',1,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('b','CRITICAL','NOW','SHARED','BUG','desc',2,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('c','MAJOR','IN_VIEW','CORE','BUG','desc',3,'OPEN')`)

	items, err := queryBacklogList(bm, BacklogFilter{Severity: []string{"CRITICAL"}})
	if err != nil {
		t.Fatalf("queryBacklogList: %v", err)
	}
	if len(items) != 2 {
		t.Errorf("expected 2 critical items, got %d", len(items))
	}

	items2, err := queryBacklogList(bm, BacklogFilter{Scope: []string{"CORE"}})
	if err != nil {
		t.Fatalf("queryBacklogList: %v", err)
	}
	if len(items2) != 2 {
		t.Errorf("expected 2 CORE items, got %d", len(items2))
	}
}

func TestQueryBacklogList_Sort(t *testing.T) {
	st, bm, _, _ := setupTestQueriers(t)

	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('first','MAJOR','NOW','CORE','BUG','desc',1,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('second','MINOR','NOW','CORE','BUG','desc',2,'OPEN')`)

	items, err := queryBacklogList(bm, BacklogFilter{SortBy: "id", SortDir: "ASC"})
	if err != nil {
		t.Fatalf("queryBacklogList: %v", err)
	}
	if len(items) != 2 {
		t.Fatalf("expected 2 items, got %d", len(items))
	}
	if items[0].Title != "first" {
		t.Errorf("expected first item 'first', got '%s'", items[0].Title)
	}
}

func TestQueryActiveBranches(t *testing.T) {
	st, _, hm, _ := setupTestQueriers(t)

	st.Exec(`INSERT INTO active_branches (branch,workspace,git_branch,status,summary)
		VALUES ('b1','ws1','feature/test','IMPLEMENTING','test branch')`)
	st.Exec(`INSERT INTO branch_backlogs (branch,backlog_id) VALUES ('b1',42)`)
	st.Exec(`INSERT INTO branch_backlogs (branch,backlog_id) VALUES ('b1',43)`)

	branches, err := queryActiveBranches(hm)
	if err != nil {
		t.Fatalf("queryActiveBranches: %v", err)
	}
	if len(branches) != 1 {
		t.Fatalf("expected 1 branch, got %d", len(branches))
	}
	if branches[0].GitBranch != "feature/test" {
		t.Errorf("expected feature/test, got %s", branches[0].GitBranch)
	}
	if len(branches[0].BacklogIDs) != 2 {
		t.Errorf("expected 2 backlogs, got %d", len(branches[0].BacklogIDs))
	}
}

func TestQueryQueueStatus_Empty(t *testing.T) {
	_, _, _, qm := setupTestQueriers(t)

	entries, err := queryQueueStatus(qm)
	if err != nil {
		t.Fatalf("queryQueueStatus: %v", err)
	}
	if len(entries) != 0 {
		t.Errorf("expected 0 entries, got %d", len(entries))
	}
}

func TestQueryBranchHistory(t *testing.T) {
	st, _, hm, _ := setupTestQueriers(t)

	st.Exec(`INSERT INTO branch_history (branch,workspace,git_branch,status,summary,backlog_ids,started_at)
		VALUES ('b1','ws1','feature/done','MERGED','completed','[1,2]','2026-01-01 00:00:00')`)

	history, err := queryBranchHistory(hm, 10)
	if err != nil {
		t.Fatalf("queryBranchHistory: %v", err)
	}
	if len(history) != 1 {
		t.Fatalf("expected 1, got %d", len(history))
	}
	if history[0].Status != "MERGED" {
		t.Errorf("expected MERGED, got %s", history[0].Status)
	}
}
