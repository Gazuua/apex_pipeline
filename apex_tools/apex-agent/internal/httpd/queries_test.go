// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupTestStore(t *testing.T) *store.Store {
	t.Helper()
	st, err := store.Open(":memory:")
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { st.Close() })

	// Create tables directly (mirrors module migrations)
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
			created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`,
	}
	for _, ddl := range tables {
		if _, err := st.Exec(ddl); err != nil {
			t.Fatalf("create table: %v", err)
		}
	}
	return st
}

func TestQueryDashboardSummary_Empty(t *testing.T) {
	st := setupTestStore(t)
	s, err := queryDashboardSummary(st)
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
	st := setupTestStore(t)

	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('bug1','CRITICAL','NOW','CORE','BUG','desc',1,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('bug2','MAJOR','NOW','CORE','BUG','desc',2,'FIXING')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('bug3','MINOR','IN_VIEW','SHARED','BUG','desc',3,'RESOLVED')`)

	s, err := queryDashboardSummary(st)
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
	st := setupTestStore(t)

	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('a','CRITICAL','NOW','CORE','BUG','desc',1,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('b','CRITICAL','NOW','SHARED','BUG','desc',2,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('c','MAJOR','IN_VIEW','CORE','BUG','desc',3,'OPEN')`)

	items, err := queryBacklogList(st, BacklogFilter{Severity: "CRITICAL"})
	if err != nil {
		t.Fatalf("queryBacklogList: %v", err)
	}
	if len(items) != 2 {
		t.Errorf("expected 2 critical items, got %d", len(items))
	}

	items2, err := queryBacklogList(st, BacklogFilter{Scope: "CORE"})
	if err != nil {
		t.Fatalf("queryBacklogList: %v", err)
	}
	if len(items2) != 2 {
		t.Errorf("expected 2 CORE items, got %d", len(items2))
	}
}

func TestQueryBacklogList_Sort(t *testing.T) {
	st := setupTestStore(t)

	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('first','MAJOR','NOW','CORE','BUG','desc',1,'OPEN')`)
	st.Exec(`INSERT INTO backlog_items (title,severity,timeframe,scope,type,description,position,status)
		VALUES ('second','MINOR','NOW','CORE','BUG','desc',2,'OPEN')`)

	items, err := queryBacklogList(st, BacklogFilter{SortBy: "id", SortDir: "ASC"})
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
	st := setupTestStore(t)

	st.Exec(`INSERT INTO active_branches (branch,workspace,git_branch,status,summary)
		VALUES ('b1','ws1','feature/test','IMPLEMENTING','test branch')`)
	st.Exec(`INSERT INTO branch_backlogs (branch,backlog_id) VALUES ('b1',42)`)
	st.Exec(`INSERT INTO branch_backlogs (branch,backlog_id) VALUES ('b1',43)`)

	branches, err := queryActiveBranches(st)
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
	st := setupTestStore(t)

	entries, err := queryQueueStatus(st)
	if err != nil {
		t.Fatalf("queryQueueStatus: %v", err)
	}
	if len(entries) != 0 {
		t.Errorf("expected 0 entries, got %d", len(entries))
	}
}

func TestQueryBranchHistory(t *testing.T) {
	st := setupTestStore(t)

	st.Exec(`INSERT INTO branch_history (branch,workspace,git_branch,status,summary,backlog_ids,started_at)
		VALUES ('b1','ws1','feature/done','MERGED','completed','[1,2]','2026-01-01 00:00:00')`)

	history, err := queryBranchHistory(st, 10)
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
